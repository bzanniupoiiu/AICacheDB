// kvs_repli_slave.c
#include "kvstore.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <rdma/rdma_cma.h>
#include "rdma.h"


static int slave_tcp_fd = -1;

static pthread_t slave_tcp_thread;

static volatile int slave_running = 1;

extern int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len) ;

static void on_rdma_recv_complete(struct ibv_wc *wc) {
    conn_manger_t *conn = (conn_manger_t *)wc->wr_id;
    if (wc->status != IBV_WC_SUCCESS) {
        if (wc->status != IBV_WC_WR_FLUSH_ERR) {
            fprintf(stderr, "RDMA recv error: %s\n", ibv_wc_status_str(wc->status));
        }
        if (conn) rdma_disconnect(conn->cm_id);
        return;
    }

    if (wc->opcode == IBV_WC_RECV) {
        static int rdb_fd = -1;
        static off_t rdb_offset = 0;

        if (rdb_fd == -1) {
            mkdir("./Persistence", 0755);
            rdb_fd = open("./Persistence/kvstore.rdb", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rdb_fd < 0) {
                perror("open RDB file");
                rdma_disconnect(conn->cm_id);
                return;
            }
            printf("Started receiving RDB\n");
        }

        // 解析消息头
        msg_header_t *hdr = (msg_header_t *)conn->recv_buffer;
        uint32_t data_len = hdr->len;
        int is_last = hdr->is_last;

        // 写入实际数据（跳过头部）
        ssize_t n = write(rdb_fd, conn->recv_buffer + sizeof(msg_header_t), data_len);
        if (n != data_len) {
            perror("write failed");
            close(rdb_fd);
            rdb_fd = -1;
            rdma_disconnect(conn->cm_id);
            return;
        }
        rdb_offset += n;

        // 处理最后一块
        if (is_last) {
            close(rdb_fd);
            printf("RDB receive complete, %ld bytes\n", (long)rdb_offset);
            if (rdb_load_files() != 0) {
                fprintf(stderr, "Failed to load RDB\n");
            } else {
                printf("RDB loaded\n");
            }
            rdb_fd = -1;
            rdb_offset = 0;

            // 发送 DONE
            const char *done = "DONE";
            memcpy(conn->send_buffer, done, 4);
            if (post_send(conn, 4) != 0) {
                fprintf(stderr, "Failed to send DONE\n");
                rdma_disconnect(conn->cm_id);
                return;
            }

            char ack_buf[16];
            ssize_t n = recv(slave_tcp_fd, ack_buf, sizeof(ack_buf)-1, 0);
            if (n > 0 && strncmp(ack_buf, "+DONE", 5) == 0) {
                // 发送 FINSYNC
                const char *finsync = "*1\r\n$7\r\nFINSYNC\r\n";
                send(slave_tcp_fd, finsync, strlen(finsync), 0);
            }
            
            // 可选：断开 RDMA 连接（主节点不会主动断开）
            rdma_disconnect(conn->cm_id);
            conn->running = 0;
        } else {
            // 发送 ACK
            const char *ack = "ACK";
            memcpy(conn->send_buffer, ack, 3);
            if (post_send(conn, 3) != 0) {
                fprintf(stderr, "Failed to send ACK\n");
                rdma_disconnect(conn->cm_id);
                return;
            }
        }

        // 重新贴一个接收，准备接收下一块（如果是最后一块且不再需要接收，可以不贴）
        if (!is_last) {
            post_recv(conn);
        }
    }
}
static void *slave_tcp_receive_thread(void *arg) {
    int fd = (int)(long)arg;

    if (fd <= 0) {
        printf("Invalid fd %d, exiting thread\n", fd);
        return NULL;
    }

    char *rbuf = kvs_malloc(4096);
    size_t rbuf_size = 4096;
    int rlen = 0;

    char *resp = NULL;

    printf("slave_tcp_receive_thread started, fd=%d\n", fd);

    while (slave_running) {

        /* ===== 自动扩容 ===== */
        if (rlen >= (int)rbuf_size - 1) {
            rbuf_size *= 2;

            char *new_buf = realloc(rbuf, rbuf_size);
            if (!new_buf) {
                fprintf(stderr, "Failed to realloc buffer\n");
                break;
            }

            rbuf = new_buf;
        }

        ssize_t n = recv(fd, rbuf + rlen, rbuf_size - rlen - 1, 0);

        if (n <= 0) {
            if (n == 0) {
                printf("Connection closed by peer, fd=%d\n", fd);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            } else {
                perror("recv");
            }
            break;
        }

        rlen += n;

        /* ===== RESP 流式解析 ===== */
        int parse_offset = 0;

        while (parse_offset < rlen) {

            size_t cmd_len = 0;
            int ret = resp_parse_one_command(
                rbuf + parse_offset,
                rlen - parse_offset,
                &cmd_len
            );

            /* ===== 1. 成功解析 ===== */
            if (ret == 1) {

                /* 防御：非法长度 */
                if (cmd_len == 0 ||
                    cmd_len > (size_t)(rlen - parse_offset)) {

                    fprintf(stderr,
                        "Invalid cmd_len=%zu, available=%d\n",
                        cmd_len, rlen - parse_offset);

                    /* 丢弃当前 buffer（防止错位扩散） */
                    parse_offset = rlen;
                    break;
                }

                replicating = 1;
                g_config.slave_mode = 0;

                int ret2 = kvs_protocol(
                    fd,
                    rbuf + parse_offset,
                    cmd_len,
                    &resp
                );

                if (resp) {
                    kvs_free(resp);
                    resp = NULL;
                }

                g_config.slave_mode = 1;
                replicating = 0;

                parse_offset += cmd_len;
                continue;
            }

            /* ===== 2. 数据不完整 ===== */
            if (ret == 0) {
                break;
            }

            /* ===== 3. 协议错误（关键修复点） ===== */
            if (ret < 0) {

                fprintf(stderr,
                        "RESP parse error at offset %d (NOT closing fd=%d)\n",
                        parse_offset, fd);

                /* debug dump */
                int debug_len = (rlen - parse_offset) < 64 ?
                                (rlen - parse_offset) : 64;

                fprintf(stderr, "Dump: ");
                for (int i = 0; i < debug_len; i++) {
                    fprintf(stderr, "%02x ",
                        (unsigned char)rbuf[parse_offset + i]);
                }
                fprintf(stderr, "\n");

                /* ===== 修复策略：丢弃当前 buffer，而不是关闭连接 ===== */
                parse_offset = rlen;
                break;
            }
        }

        /* ===== 移动未处理数据 ===== */
        if (parse_offset > 0) {

            if (parse_offset < rlen) {
                memmove(rbuf,
                        rbuf + parse_offset,
                        rlen - parse_offset);
            }

            rlen -= parse_offset;
        }
    }

    kvs_free(rbuf);
    close(fd);

    printf("slave_tcp_receive_thread exiting, fd=%d\n", fd);
    return NULL;
}
static void *accept_ebpf_conn(void *arg) {
    int lfd = (int)(long)arg;
    while (1) {
        int client = accept(lfd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        // printf("eBPF forwarder connected, fd=%d\n", client);
        // 启动一个线程处理该连接（使用修改后的 slave_tcp_receive_thread）
        pthread_t th;
        pthread_create(&th, NULL, slave_tcp_receive_thread, (void*)(long)client);
        pthread_detach(th);
    }
    close(lfd);
    return NULL;
}


int kvs_replication_slave_start(const char *master_host, int master_port, int rdma_port) {

    // TCP 连接主机
    int tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) return -1;
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(master_port);
    inet_pton(AF_INET, master_host, &addr.sin_addr);
    if (connect(tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(tcp_fd);
        return -1;
    }
    slave_tcp_fd = tcp_fd;

    // 发送 SYNC并检查返回值是不是OK
    const char *sync_cmd = "*1\r\n$4\r\nSYNC\r\n";
    if (send(tcp_fd, sync_cmd, strlen(sync_cmd), 0) != strlen(sync_cmd)) {
        close(tcp_fd);
        return -1;
    }
    char ok_buf[16];
    ssize_t n = recv(tcp_fd, ok_buf, sizeof(ok_buf) - 1, 0);
    if (n <= 0) {
        close(tcp_fd);
        return -1;
    }
    ok_buf[n] = '\0';
    if (strcmp(ok_buf, "+OK\r\n") != 0) {
        fprintf(stderr, "Unexpected SYNC reply: %s", ok_buf);
        close(tcp_fd);
        return -1;
    }

    int repl_port = 8000;
    // 3. 创建监听 socket 用于接收 eBPF 转发的增量命令（必须在 RDMA 之前）
    printf("Slave listening on port %d for eBPF forwarder\n", repl_port);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        close(tcp_fd);
        return -1;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(repl_port),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        close(tcp_fd);
    }
    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        close(tcp_fd);
    }
    fflush(stdout);   // 立即输出
    pthread_t accept_th;
    pthread_create(&accept_th, NULL, accept_ebpf_conn, (void*)(long)listen_fd);
    pthread_detach(accept_th);


    // RDMA 连接
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *cm_id;
    if (rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        close(tcp_fd);
        return -1;
    }
    struct sockaddr_in rdma_addr;
    rdma_addr.sin_family = AF_INET;
    rdma_addr.sin_port = htons(rdma_port);
    inet_pton(AF_INET, master_host, &rdma_addr.sin_addr);
    if (rdma_resolve_addr(cm_id, NULL, (struct sockaddr*)&rdma_addr, 2000)) {
        perror("rdma_resolve_addr");
        goto err;
    }

    struct rdma_cm_event *event;
    int connected = 0;
    conn_manger_t *conn = NULL;
    while (rdma_get_cm_event(ec, &event) == 0) {
        switch (event->event) {
            case RDMA_CM_EVENT_ADDR_RESOLVED:
                if (rdma_resolve_route(cm_id, 2000)) goto err;
                break;
            case RDMA_CM_EVENT_ROUTE_RESOLVED:
                // 传入接收回调
                initialize_connection(cm_id, on_rdma_recv_complete);
                if (rdma_connect(cm_id, NULL)) goto err;
                break;
            case RDMA_CM_EVENT_ESTABLISHED:
                connected = 1;
                conn = cm_id->context;
                // 预贴接收
                post_recv(conn);
                break;
            case RDMA_CM_EVENT_DISCONNECTED:
                destory_connection(cm_id);
                rdma_destroy_event_channel(ec);
                break;
            default:
                break;
        }
        rdma_ack_cm_event(event);
        if (connected) break;
    }
    if (!connected) goto err;

    // 等待 RDMA 传输完成（直到连接断开）
    while (conn && conn->running) ;

    return 0;

err:
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);
    close(tcp_fd);
    return -1;
}

void kvs_replication_slave_cleanup(void) {
    slave_running = 0;
    if (slave_tcp_fd >= 0) {
        close(slave_tcp_fd);
        slave_tcp_fd = -1;
    }
}
