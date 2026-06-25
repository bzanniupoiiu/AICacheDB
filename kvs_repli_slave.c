// kvs_repli_slave.c - enterprise RDMA full-sync path
#include "kvstore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rdma.h"

#define SLAVE_RDB_FILE     "./Persistence/kvstore.rdb"
#define SLAVE_RDB_TMP_FILE "./Persistence/kvstore.rdb.tmp"

static int slave_tcp_fd = -1;
static volatile int slave_running = 1;
static int slave_ebpf_listen_fd = -1;

extern int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len);

typedef struct {
    int rdb_fd;
    off_t rdb_offset;
    uint64_t next_seq;
    uint64_t last_acked_seq;
    int done_posted;
    uint64_t done_seq;
} slave_rdb_state_t;

static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

static int recv_until_token(int fd, const char *token)
{
    char buf[128];
    int len = 0;
    size_t token_len = strlen(token);

    while (len < (int)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        len += (int)n;
        buf[len] = '\0';
        if (len >= (int)token_len && strstr(buf, token) != NULL) {
            return 0;
        }
    }

    return -1;
}

static int recv_master_done(int fd)
{
    return recv_until_token(fd, "+DONE\r\n");
}

static int send_repl_ack(int fd)
{
    const char ack[] = "+ACK\r\n";
    return send_all(fd, ack, sizeof(ack) - 1);
}

static int write_full(int fd, const char *buf, size_t len)
{
    size_t written = 0;

    while (written < len) {
        ssize_t n = write(fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        written += (size_t)n;
    }

    return 0;
}

static int open_slave_rdb(slave_rdb_state_t *state)
{
    if (state->rdb_fd >= 0) {
        return 0;
    }

    mkdir("./Persistence", 0755);
    /* 全量同步先写临时文件，最后一片确认后再 rename，避免半包数据破坏旧快照。 */
    state->rdb_fd = open(SLAVE_RDB_TMP_FILE,
                         O_WRONLY | O_CREAT | O_TRUNC,
                         0644);
    if (state->rdb_fd < 0) {
        perror("open slave RDB");
        return -1;
    }

    return 0;
}

static void close_slave_rdb(slave_rdb_state_t *state)
{
    if (state && state->rdb_fd >= 0) {
        close(state->rdb_fd);
        state->rdb_fd = -1;
    }
}

static int commit_slave_rdb(slave_rdb_state_t *state, uint64_t total_size)
{
    if (!state) {
        return -1;
    }

    if (state->rdb_fd >= 0) {
        if (fsync(state->rdb_fd) != 0) {
            perror("fsync slave RDB");
            close_slave_rdb(state);
            return -1;
        }
        close_slave_rdb(state);
    }

    if (total_size == 0) {
        unlink(SLAVE_RDB_TMP_FILE);
        unlink(SLAVE_RDB_FILE);
        return 0;
    }

    if (rename(SLAVE_RDB_TMP_FILE, SLAVE_RDB_FILE) != 0) {
        perror("rename slave RDB");
        return -1;
    }

    return 0;
}

static int load_received_rdb(uint64_t total_size)
{
    if (total_size == 0) {
        return kvs_reset_data_engines();
    }

    return rdb_load_files();
}

static int slave_post_ack(conn_manger_t *conn,
                          slave_rdb_state_t *state,
                          uint64_t seq,
                          uint64_t offset)
{
    uint32_t slot = (uint32_t)(seq % RDMA_REPL_SEND_SLOTS);
    if (post_ctrl_slot(conn, slot, RDMA_CTRL_ACK, seq, offset, 0) != 0) {
        return -1;
    }
    state->last_acked_seq = seq;
    return 0;
}

static int slave_post_error(conn_manger_t *conn,
                            uint64_t seq,
                            uint64_t offset,
                            uint32_t status)
{
    uint32_t slot = (uint32_t)(seq % RDMA_REPL_SEND_SLOTS);
    post_ctrl_slot(conn, slot, RDMA_CTRL_ERROR, seq, offset, status);
    return -1;
}

static int process_rdma_chunk(conn_manger_t *conn,
                              slave_rdb_state_t *state,
                              uint32_t data_slot)
{
    rdma_chunk_header_t *hdr;
    char *slot_base;
    char *payload;

    if (!conn || !state) {
        return -1;
    }
    if (data_slot >= conn->local_slot_count) {
        fprintf(stderr, "RDMA data slot out of range: slot=%u count=%u\n",
                data_slot, conn->local_slot_count);
        return slave_post_error(conn, state->next_seq, state->rdb_offset, 100);
    }

    slot_base = rdma_data_slot_ptr(conn, data_slot);
    if (!slot_base) {
        return -1;
    }

    hdr = (rdma_chunk_header_t *)slot_base;
    payload = slot_base + sizeof(*hdr);

    if (hdr->magic != RDMA_REPL_MAGIC || hdr->version != RDMA_REPL_VERSION) {
        fprintf(stderr, "invalid RDMA chunk header: slot=%u magic=%x version=%u\n",
                data_slot, hdr->magic, hdr->version);
        return slave_post_error(conn, state->next_seq, state->rdb_offset, 101);
    }

    if (hdr->header_len != sizeof(*hdr)) {
        fprintf(stderr, "RDMA header size mismatch: got=%u expected=%zu\n",
                hdr->header_len, sizeof(*hdr));
        return slave_post_error(conn, hdr->seq, state->rdb_offset, 102);
    }

    if (hdr->slot_id != data_slot) {
        fprintf(stderr, "RDMA slot mismatch: header=%u imm=%u\n",
                hdr->slot_id, data_slot);
        return slave_post_error(conn, hdr->seq, state->rdb_offset, 103);
    }

    if (hdr->seq != state->next_seq) {
        fprintf(stderr,
                "RDMA chunk seq mismatch: got=%llu expected=%llu\n",
                (unsigned long long)hdr->seq,
                (unsigned long long)state->next_seq);
        return slave_post_error(conn, hdr->seq, state->rdb_offset, 104);
    }

    if (hdr->len > conn->local_slot_size - sizeof(*hdr)) {
        fprintf(stderr, "RDMA chunk len too large: %u slot_size=%u\n",
                hdr->len, conn->local_slot_size);
        return slave_post_error(conn, hdr->seq, state->rdb_offset, 105);
    }

    if ((uint64_t)state->rdb_offset != hdr->offset) {
        fprintf(stderr,
                "RDMA chunk offset mismatch: got=%llu expected=%lld\n",
                (unsigned long long)hdr->offset,
                (long long)state->rdb_offset);
        return slave_post_error(conn, hdr->seq, state->rdb_offset, 106);
    }

    if (hdr->len > 0) {
        uint64_t actual_checksum = rdma_fnv1a64(payload, hdr->len);
        if (actual_checksum != hdr->checksum) {
            fprintf(stderr,
                    "RDMA chunk checksum mismatch: seq=%llu got=%llx expected=%llx\n",
                    (unsigned long long)hdr->seq,
                    (unsigned long long)actual_checksum,
                    (unsigned long long)hdr->checksum);
            return slave_post_error(conn, hdr->seq, state->rdb_offset, 107);
        }

        if (open_slave_rdb(state) != 0) {
            return slave_post_error(conn, hdr->seq, state->rdb_offset, 108);
        }

        if (write_full(state->rdb_fd, payload, hdr->len) != 0) {
            perror("write slave RDB");
            return slave_post_error(conn, hdr->seq, state->rdb_offset, 109);
        }
    }

    state->rdb_offset += hdr->len;

    if (hdr->is_last) {
        uint32_t ctrl_slot = (uint32_t)(hdr->seq % RDMA_REPL_SEND_SLOTS);

        if (commit_slave_rdb(state, hdr->total_size) != 0) {
            return slave_post_error(conn, hdr->seq, state->rdb_offset, 110);
        }

        if (load_received_rdb(hdr->total_size) != 0) {
            fprintf(stderr, "load received RDB failed\n");
            return slave_post_error(conn, hdr->seq, state->rdb_offset, 111);
        }

        if (post_ctrl_slot(conn,
                           ctrl_slot,
                           RDMA_CTRL_DONE,
                           hdr->seq,
                           state->rdb_offset,
                           0) != 0) {
            return -1;
        }

        state->done_posted = 1;
        state->done_seq = hdr->seq;
        return 0;
    }

    state->next_seq++;

    if (hdr->seq - state->last_acked_seq >= RDMA_REPL_ACK_EVERY) {
        if (slave_post_ack(conn, state, hdr->seq, state->rdb_offset) != 0) {
            return -1;
        }
    }

    return 0;
}

static void on_rdma_recv_complete(struct ibv_wc *wc)
{
    rdma_wr_ctx_t *ctx = rdma_wc_ctx(wc);
    conn_manger_t *conn = rdma_wc_conn(wc);
    slave_rdb_state_t *state;
    uint32_t data_slot;

    if (!conn || !ctx) {
        return;
    }

    if (wc->status != IBV_WC_SUCCESS) {
        if (wc->status != IBV_WC_WR_FLUSH_ERR) {
            fprintf(stderr,
                    "RDMA completion error: %s\n",
                    ibv_wc_status_str(wc->status));
        }
        rdma_mark_failed(conn);
        rdma_disconnect(conn->cm_id);
        return;
    }

    state = (slave_rdb_state_t *)conn->user_data;

    if (ctx->type == RDMA_WR_CTX_SEND) {
        conn->completed_send++;
        if (state && state->done_posted && ctx->seq == state->done_seq) {
            rdma_mark_done(conn);
        }
        return;
    }

    if (ctx->type != RDMA_WR_CTX_RECV) {
        return;
    }

    conn->completed_recv++;

    if (wc->opcode != IBV_WC_RECV) {
        return;
    }

    rdma_ctrl_msg_t *ctrl = (rdma_ctrl_msg_t *)rdma_ctrl_recv_slot_ptr(conn, ctx->slot);
    if (!ctrl || ctrl->magic != RDMA_REPL_MAGIC || ctrl->type != RDMA_CTRL_CHUNK) {
        fprintf(stderr, "invalid RDMA chunk notify\n");
        rdma_mark_failed(conn);
        rdma_disconnect(conn->cm_id);
        return;
    }

    data_slot = (uint32_t)ctrl->offset;
    if (process_rdma_chunk(conn, state, data_slot) != 0) {
        rdma_mark_failed(conn);
        rdma_disconnect(conn->cm_id);
        return;
    }

    if (!state->done_posted) {
        if (post_recv_slot(conn, ctx->slot) != 0) {
            rdma_mark_failed(conn);
            return;
        }
    }
}

static void *slave_tcp_receive_thread(void *arg)
{
    int fd = (int)(long)arg;
    char *rbuf = NULL;
    size_t rbuf_size = 4096;
    int rlen = 0;
    char *resp = NULL;

    if (fd <= 0) {
        return NULL;
    }

    rbuf = (char *)kvs_malloc(rbuf_size);
    if (!rbuf) {
        close(fd);
        return NULL;
    }

    printf("slave eBPF receive thread started, fd=%d\n", fd);

    while (slave_running) {
        if (rlen >= (int)rbuf_size - 1) {
            rbuf_size *= 2;
            char *new_buf = (char *)realloc(rbuf, rbuf_size);
            if (!new_buf) {
                fprintf(stderr, "failed to realloc eBPF recv buffer\n");
                break;
            }
            rbuf = new_buf;
        }

        ssize_t n = recv(fd, rbuf + rlen, rbuf_size - rlen - 1, 0);
        if (n <= 0) {
            if (n == 0) {
                printf("eBPF forwarder closed, fd=%d\n", fd);
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            } else {
                perror("recv eBPF command");
            }
            break;
        }

        rlen += (int)n;

        int parse_offset = 0;
        while (parse_offset < rlen) {
            size_t cmd_len = 0;
            int ret = resp_parse_one_command(rbuf + parse_offset,
                                             (size_t)(rlen - parse_offset),
                                             &cmd_len);

            if (ret == 1) {
                if (cmd_len == 0 || cmd_len > (size_t)(rlen - parse_offset)) {
                    fprintf(stderr, "invalid replicated cmd_len=%zu\n", cmd_len);
                    parse_offset = rlen;
                    break;
                }

                replicating = 1;
                int resp_len = kvs_protocol(fd,
                                            rbuf + parse_offset,
                                            cmd_len,
                                            &resp);
                replicating = 0;

                int apply_failed = resp_len < 0 ||
                                   (resp_len > 0 && resp && resp[0] == '-');
                if (apply_failed && resp && resp_len > 0) {
                    fprintf(stderr,
                            "replicated command returned error: %.*s\n",
                            resp_len,
                            resp);
                }

                if (resp) {
                    kvs_free(resp);
                    resp = NULL;
                }

                if (apply_failed || send_repl_ack(fd) != 0) {
                    fprintf(stderr, "apply replicated command failed\n");
                    parse_offset = rlen;
                    slave_running = 0;
                    break;
                }

                parse_offset += (int)cmd_len;
                continue;
            }

            if (ret == 0) {
                break;
            }

            fprintf(stderr, "RESP parse error from eBPF forwarder, drop buffer\n");
            parse_offset = rlen;
            break;
        }

        if (parse_offset > 0) {
            if (parse_offset < rlen) {
                memmove(rbuf, rbuf + parse_offset, (size_t)(rlen - parse_offset));
            }
            rlen -= parse_offset;
        }
    }

    if (resp) {
        kvs_free(resp);
    }
    kvs_free(rbuf);
    close(fd);

    printf("slave eBPF receive thread exiting, fd=%d\n", fd);
    return NULL;
}

static void *accept_ebpf_conn(void *arg)
{
    int lfd = (int)(long)arg;

    while (slave_running) {
        int client = accept(lfd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!slave_running) {
                break;
            }
            perror("accept eBPF forwarder");
            break;
        }

        pthread_t th;
        if (pthread_create(&th, NULL, slave_tcp_receive_thread, (void *)(long)client) == 0) {
            pthread_detach(th);
        } else {
            close(client);
        }
    }

    close(lfd);
    return NULL;
}

static int start_ebpf_listener(void)
{
    int listen_fd;
    int opt = 1;
    struct sockaddr_in listen_addr;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket eBPF listener");
        return -1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(8000);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("bind eBPF listener");
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 16) < 0) {
        perror("listen eBPF listener");
        close(listen_fd);
        return -1;
    }

    slave_ebpf_listen_fd = listen_fd;

    pthread_t accept_th;
    if (pthread_create(&accept_th, NULL, accept_ebpf_conn, (void *)(long)listen_fd) != 0) {
        close(listen_fd);
        slave_ebpf_listen_fd = -1;
        return -1;
    }
    pthread_detach(accept_th);

    printf("Slave listening on port %d for eBPF forwarder\n", 8000);
    return 0;
}

static int connect_master_tcp(const char *master_host, int master_port)
{
    int tcp_fd;
    struct sockaddr_in addr;
    const char *sync_cmd = "*1\r\n$4\r\nSYNC\r\n";

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(master_port);
    inet_pton(AF_INET, master_host, &addr.sin_addr);

    if (connect(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect master TCP");
        close(tcp_fd);
        return -1;
    }

    if (send_all(tcp_fd, sync_cmd, strlen(sync_cmd)) != 0) {
        close(tcp_fd);
        return -1;
    }

    if (recv_until_token(tcp_fd, "+OK\r\n") != 0) {
        fprintf(stderr, "unexpected SYNC reply\n");
        close(tcp_fd);
        return -1;
    }

    return tcp_fd;
}

static int connect_master_rdma(const char *master_host,
                               int rdma_port,
                               struct rdma_event_channel **out_ec,
                               struct rdma_cm_id **out_cm_id,
                               conn_manger_t **out_conn,
                               slave_rdb_state_t **out_state)
{
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *cm_id = NULL;
    conn_manger_t *conn = NULL;
    slave_rdb_state_t *state = NULL;
    int connected = 0;
    struct sockaddr_in rdma_addr;

    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        return -1;
    }

    if (rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        rdma_destroy_event_channel(ec);
        return -1;
    }

    memset(&rdma_addr, 0, sizeof(rdma_addr));
    rdma_addr.sin_family = AF_INET;
    rdma_addr.sin_port = htons(rdma_port);
    inet_pton(AF_INET, master_host, &rdma_addr.sin_addr);

    if (rdma_resolve_addr(cm_id, NULL, (struct sockaddr *)&rdma_addr, 2000)) {
        perror("rdma_resolve_addr");
        goto fail;
    }

    while (!connected) {
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(ec, &event) != 0) {
            perror("rdma_get_cm_event");
            goto fail;
        }

        if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED) {
            if (rdma_resolve_route(cm_id, 2000)) {
                rdma_ack_cm_event(event);
                goto fail;
            }
        } else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED) {
            struct rdma_conn_param param;
            rdma_peer_region_t region;

            if (initialize_connection(cm_id, on_rdma_recv_complete) != 0) {
                rdma_ack_cm_event(event);
                goto fail;
            }

            conn = (conn_manger_t *)cm_id->context;
            state = (slave_rdb_state_t *)calloc(1, sizeof(*state));
            if (!state) {
                rdma_ack_cm_event(event);
                goto fail;
            }
            state->rdb_fd = -1;
            state->next_seq = 1;
            state->last_acked_seq = 0;
            conn->user_data = state;

            if (post_recv_n(conn, RDMA_REPL_WINDOW) != 0 ||
                rdma_get_local_region(conn, &region) != 0) {
                rdma_ack_cm_event(event);
                goto fail;
            }

            memset(&param, 0, sizeof(param));
            param.private_data = &region;
            param.private_data_len = sizeof(region);

            if (rdma_connect(cm_id, &param)) {
                perror("rdma_connect");
                rdma_ack_cm_event(event);
                goto fail;
            }
        } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
            connected = 1;
        } else {
            fprintf(stderr, "unexpected RDMA event: %s\n", rdma_event_str(event->event));
            rdma_ack_cm_event(event);
            goto fail;
        }

        rdma_ack_cm_event(event);
    }

    *out_ec = ec;
    *out_cm_id = cm_id;
    *out_conn = conn;
    *out_state = state;
    return 0;

fail:
    if (state) {
        close_slave_rdb(state);
        free(state);
    }
    if (cm_id) {
        if (cm_id->context) {
            destroy_connection(cm_id);
        } else {
            rdma_destroy_id(cm_id);
        }
    }
    if (ec) {
        rdma_destroy_event_channel(ec);
    }
    return -1;
}

int kvs_replication_slave_start(const char *master_host, int master_port, int rdma_port)
{
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *cm_id = NULL;
    conn_manger_t *conn = NULL;
    slave_rdb_state_t *state = NULL;
    const char *finsync = "*1\r\n$7\r\nFINSYNC\r\n";
    int ret = -1;

    slave_running = 1;

    if (start_ebpf_listener() != 0) {
        return -1;
    }

    slave_tcp_fd = connect_master_tcp(master_host, master_port);
    if (slave_tcp_fd < 0) {
        goto out;
    }

    if (connect_master_rdma(master_host,
                            rdma_port,
                            &ec,
                            &cm_id,
                            &conn,
                            &state) != 0) {
        goto out;
    }

    rdma_wait_done(conn);

    if (conn->failed || !conn->done) {
        goto out;
    }

    if (recv_master_done(slave_tcp_fd) != 0) {
        fprintf(stderr, "failed to receive +DONE from master\n");
        goto out;
    }

    if (send_all(slave_tcp_fd, finsync, strlen(finsync)) != 0) {
        fprintf(stderr, "failed to send FINSYNC\n");
        goto out;
    }

    ret = 0;

out:
    if (cm_id) {
        rdma_disconnect(cm_id);
        if (cm_id->context) {
            destroy_connection(cm_id);
        } else {
            rdma_destroy_id(cm_id);
        }
    }
    if (ec) {
        rdma_destroy_event_channel(ec);
    }
    if (state) {
        close_slave_rdb(state);
        free(state);
    }
    if (ret != 0 && slave_tcp_fd >= 0) {
        close(slave_tcp_fd);
        slave_tcp_fd = -1;
    }
    if (ret != 0) {
        slave_running = 0;
        if (slave_ebpf_listen_fd >= 0) {
            close(slave_ebpf_listen_fd);
            slave_ebpf_listen_fd = -1;
        }
    }

    return ret;
}

void kvs_replication_slave_cleanup(void)
{
    slave_running = 0;

    if (slave_ebpf_listen_fd >= 0) {
        close(slave_ebpf_listen_fd);
        slave_ebpf_listen_fd = -1;
    }

    if (slave_tcp_fd >= 0) {
        close(slave_tcp_fd);
        slave_tcp_fd = -1;
    }
}
