// kvs_repli_master.c
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

#include <sys/un.h>
#include "rdma.h"


// 全局从链表
kvs_replica_t *g_replicas = NULL;

static int rdma_running = 0;
static pthread_t rdma_listen_thread;

// 辅助函数：添加从节点
static kvs_replica_t* add_replica(int tcp_fd, struct sockaddr_in *addr, replica_state_t state) {
    kvs_replica_t *r = calloc(1, sizeof(kvs_replica_t));
    if (!r) return NULL;
    r->tcp_fd = tcp_fd;
    memcpy(&r->tcp_addr, addr, sizeof(*addr));
    r->state = state;
    pthread_mutex_init(&r->queue_mutex, NULL);
    r->cmd_queue = NULL;
    r->cmd_queue_size = 0;
    r->cmd_queue_cap = 0;
    r->rdma_cm_id = NULL;
    r->rdb_fd = -1;        // 初始化
    r->next = g_replicas;
    g_replicas = r;
    return r;
}


// 根据 TCP fd 查找从节点
static kvs_replica_t* find_replica_by_tcp_fd(int tcp_fd) {
    kvs_replica_t *r = g_replicas;
    while (r) {
        if (r->tcp_fd == tcp_fd) return r;
        r = r->next;
    }
    return NULL;
}

// 根据 RDMA cm_id 查找从节点
static kvs_replica_t* find_replica_by_rdma_cm_id(struct rdma_cm_id *cm_id) {
    kvs_replica_t *r = g_replicas;
    while (r) {
        if (r->rdma_cm_id == cm_id) return r;
        r = r->next;
    }
    return NULL;
}


// 移除从节点
static void remove_replica(kvs_replica_t *replica) {
    kvs_replica_t **pp = &g_replicas;
    while (*pp) {
        if (*pp == replica) {
            *pp = replica->next;
            if (replica->tcp_fd >= 0) close(replica->tcp_fd);
            if (replica->cmd_queue) free(replica->cmd_queue);
            if (replica->rdb_fd >= 0) close(replica->rdb_fd);
            pthread_mutex_destroy(&replica->queue_mutex);
            free(replica);
            return;
        }
        pp = &(*pp)->next;
    }
}





// 主节点收到 SYNC 命令
void kvs_replication_master_handle_sync(int tcp_fd, struct sockaddr_in *addr) {
    kvs_replica_t *replica = add_replica(tcp_fd, addr, KVS_REPL_STATE_WAIT_RDMA);
    if (!replica) {
        close(tcp_fd);
        return;
    }
    const char *ok = "+OK\r\n";
    send(tcp_fd, ok, strlen(ok), 0);
    
}



// 主节点收到 FINSYNC 命令
void kvs_replication_master_handle_finsync(int tcp_fd) {
    kvs_replica_t *replica = find_replica_by_tcp_fd(tcp_fd);
    if (!replica) return;
    if (replica->state != KVS_REPL_STATE_WAIT_FINSYNC) return;
    replica->state = KVS_REPL_STATE_ONLINE;
    
}




// ---------- RDMA 回调函数 ----------


static void master_send_chunk(kvs_replica_t *replica) 
{
    if (replica->rdb_size == 0 && replica->rdb_offset == 0) {
        conn_manger_t *conn = replica->rdma_cm_id->context;
        if (!conn) return;
        msg_header_t *hdr = (msg_header_t *)conn->send_buffer;
        hdr->len = 0;
        hdr->is_last = 1;
        if (post_send(conn, sizeof(msg_header_t)) != 0) {
            perror("post_send empty");
            rdma_disconnect(replica->rdma_cm_id);
            return;
        }
        replica->rdb_offset = replica->rdb_size; // 标记完成
        return;
    }
    conn_manger_t *conn = replica->rdma_cm_id->context;
    if (!conn) return;

    if (replica->rdb_offset >= replica->rdb_size) {
        // 全部发完，等待从节点回复 DONE
        return;
    }

    size_t chunk_size = BUFFER_SIZE - sizeof(msg_header_t);
    size_t to_send = chunk_size;
    if (replica->rdb_offset + to_send > replica->rdb_size)
        to_send = replica->rdb_size - replica->rdb_offset;//判断是不是最后一块的大小

    msg_header_t *hdr = (msg_header_t *)conn->send_buffer;
    char *data = conn->send_buffer + sizeof(msg_header_t);

    lseek(replica->rdb_fd, replica->rdb_offset, SEEK_SET);

    ssize_t n = read(replica->rdb_fd, data, to_send);//读到data里面
    if (n <= 0) {
        perror("read RDB");
        rdma_disconnect(replica->rdma_cm_id);
        return;
    }

    hdr->len = n;
    hdr->is_last = (replica->rdb_offset + n >= replica->rdb_size);

    if (post_send(conn, n + sizeof(msg_header_t)) != 0) {
        perror("post_send");
        rdma_disconnect(replica->rdma_cm_id);
        return;
    }

    replica->rdb_offset += n;
    printf("Sent %zd bytes, total %lld/%lld\n", n,
           (long long)replica->rdb_offset, (long long)replica->rdb_size);
}

// RDMA 发送完成回调（无动作，等待 ACK）
static void on_master_send_complete(struct ibv_wc *wc) {
    conn_manger_t *conn = (conn_manger_t *)wc->wr_id;
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Send error: %s\n", ibv_wc_status_str(wc->status));
        rdma_disconnect(conn->cm_id);
    }
}


// RDMA 接收完成回调（处理 ACK / DONE）
static void on_master_recv_complete(struct ibv_wc *wc) {
    conn_manger_t *conn = (conn_manger_t *)wc->wr_id;
    if (wc->status != IBV_WC_SUCCESS) {
        if (wc->status != IBV_WC_WR_FLUSH_ERR)
            fprintf(stderr, "Recv error: %s\n", ibv_wc_status_str(wc->status));
        rdma_disconnect(conn->cm_id);
        return;
    }

    kvs_replica_t *replica = (kvs_replica_t *)conn->user_data;
    if (!replica) {
        rdma_disconnect(conn->cm_id);
        return;
    }

    char *msg = conn->recv_buffer;
    if (strncmp(msg, "ACK", 3) == 0) {//接收完成后
        // 收到 ACK，继续发送下一块
        if (replica->rdb_offset < replica->rdb_size)
            master_send_chunk(replica);
        // 否则等待 DONE
    } else if (strncmp(msg, "DONE", 4) == 0) {
        // 传输完成，关闭文件并改变状态
        if (replica->rdb_fd >= 0) {
            close(replica->rdb_fd);
            replica->rdb_fd = -1;
            // 删除临时文件
            char tmp_file[256];
            snprintf(tmp_file, sizeof(tmp_file), "./Persistence/rdb_temp_%p.rdb", (void*)replica);
            unlink(tmp_file);
        }
        replica->state = KVS_REPL_STATE_WAIT_FINSYNC;
        // 可以断开 RDMA 连接，这里选择发送完不断开
        // rdma_disconnect(conn->cm_id);
        send(replica->tcp_fd, "+DONE\r\n", 7, 0);
        return;
    }

    // 重新贴一个接收
    post_recv(conn);//接收从机的应答信号
}


// 统一的完成回调（根据 opcode 分发）
static void on_master_completion(struct ibv_wc *wc) {//完成后的处理
    if (wc->opcode == IBV_WC_SEND)
        on_master_send_complete(wc);
    else if (wc->opcode == IBV_WC_RECV)
        on_master_recv_complete(wc);
}


// RDMA 连接建立时的回调（主节点侧）
static void on_rdma_connect_established(struct rdma_cm_id *cm_id) {
    struct sockaddr_in *peer = (struct sockaddr_in *)rdma_get_peer_addr(cm_id);
    kvs_replica_t *replica = g_replicas;
    while (replica) {
        //只有接收到SYNC，才能初始化replica
        if (replica->tcp_addr.sin_addr.s_addr == peer->sin_addr.s_addr && replica->state == KVS_REPL_STATE_WAIT_RDMA) 
        {
            // 关联
            replica->rdma_cm_id = cm_id;
            conn_manger_t *conn = cm_id->context;
            conn->user_data = replica;   // 让回调能找到 replica

            // 生成临时 RDB 文件
            char tmp_file[256];
            snprintf(tmp_file, sizeof(tmp_file), "./Persistence/rdb_temp_%p.rdb", (void*)replica);
            if (rdb_save_to_file(tmp_file) != 0) {
                fprintf(stderr, "Failed to save RDB\n");
                rdma_disconnect(cm_id);
                return;
            }

            // 打开文件
            replica->rdb_fd = open(tmp_file, O_RDONLY);
            if (replica->rdb_fd < 0) {
                perror("open temp RDB");
                rdma_disconnect(cm_id);
                return;
            }

            struct stat st;
            if (fstat(replica->rdb_fd, &st) != 0) {
                perror("fstat");
                close(replica->rdb_fd);
                rdma_disconnect(cm_id);
                return;
            }
            replica->rdb_size = st.st_size;
            replica->rdb_offset = 0;

            // 预贴多个接收
            for (int i = 0; i < 16; i++) {//为了和从机同步
                post_recv(conn);
            }

            // 开始发送第一块
            master_send_chunk(replica);
            return;
        }
        replica = replica->next;//这个是遍历
    }
    // 未找到对应从节点，断开连接
    rdma_disconnect(cm_id);
}



// RDMA 连接请求处理
static void on_rdma_connect_request(struct rdma_cm_id *cm_id) {
    initialize_connection(cm_id, on_master_completion);
    struct rdma_conn_param conn_param = {0};
    if (rdma_accept(cm_id, &conn_param) != 0) {
        perror("rdma_accept");
        destory_connection(cm_id);
    }
}



// RDMA 监听线程
static void *rdma_listen_thread_func(void *arg) {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id;
    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2000);
    inet_pton(AF_INET, g_config.bind_ip, &addr.sin_addr);

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr)) {
        perror("rdma_bind_addr");
        return NULL;
    }
    if (rdma_listen(listen_id, 10)) {
        fprintf(stderr, "rdma_listen failed: %s\n", strerror(errno));
        return NULL;

    }

    printf("RDMA replication listening on %s:%d\n", g_config.bind_ip, 2000);

    while (rdma_running) {
        struct rdma_cm_event *event;
        if (rdma_get_cm_event(ec, &event) == 0) {
            if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                on_rdma_connect_request(event->id);
            } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
                on_rdma_connect_established(event->id);
            } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
                
                destory_connection(event->id);
                rdma_running = 0;
            }
            rdma_ack_cm_event(event);
        }
    }
    return NULL;
}




int kvs_replication_master_init(void) {
    g_replicas = NULL;
    rdma_running = 1;
    if (pthread_create(&rdma_listen_thread, NULL, rdma_listen_thread_func, NULL) != 0) {
        perror("pthread_create");
        return -1;
    }
    pthread_detach(rdma_listen_thread);
    return 0;
}




void kvs_replication_master_cleanup(void) {
    rdma_running = 0;
    if (rdma_listen_thread) {
        pthread_cancel(rdma_listen_thread);
        pthread_join(rdma_listen_thread, NULL);
    }
    kvs_replica_t *r = g_replicas;
    while (r) {
        kvs_replica_t *next = r->next;
        if (r->tcp_fd >= 0) close(r->tcp_fd);
        if (r->cmd_queue) free(r->cmd_queue);
        if (r->rdb_fd >= 0) close(r->rdb_fd);
        pthread_mutex_destroy(&r->queue_mutex);
        free(r);
        r = next;
    }
    g_replicas = NULL;
}
