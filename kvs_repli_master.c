// kvs_repli_master.c - enterprise RDMA full-sync path
#include "kvstore.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rdma.h"

kvs_replica_t *g_replicas = NULL;

static int rdma_running = 0;
static pthread_t rdma_listen_thread;
static int rdma_listen_thread_started = 0;
static pthread_mutex_t replicas_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int in_use;
    int write_done;
    int acked;
    int is_last;
    uint32_t slot;
    uint64_t seq;
    off_t offset;
    size_t len;
} master_slot_state_t;

typedef struct {
    kvs_replica_t *replica;
    conn_manger_t *conn;
    pthread_mutex_t lock;

    uint64_t next_seq;
    off_t next_offset;
    int empty_sent;
    int done_received;
    int finishing;

    master_slot_state_t slots[RDMA_REPL_WINDOW];
} master_sync_state_t;

static void replica_free_snapshot(kvs_replica_t *replica)
{
    if (!replica) {
        return;
    }

    if (replica->rdb_mr) {
        ibv_dereg_mr(replica->rdb_mr);
        replica->rdb_mr = NULL;
    }

    if (replica->rdb_buf) {
        free(replica->rdb_buf);
        replica->rdb_buf = NULL;
    }

    replica->rdb_size = 0;
    replica->rdb_offset = 0;
    replica->rdb_seq = 0;
}

static master_sync_state_t *master_sync_state_new(kvs_replica_t *replica,
                                                  conn_manger_t *conn)
{
    master_sync_state_t *st = (master_sync_state_t *)calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    st->replica = replica;
    st->conn = conn;
    st->next_seq = 1;
    st->next_offset = 0;
    pthread_mutex_init(&st->lock, NULL);
    return st;
}

static void master_sync_state_free(master_sync_state_t *st)
{
    if (!st) {
        return;
    }
    pthread_mutex_destroy(&st->lock);
    free(st);
}

static int master_has_inflight_locked(master_sync_state_t *st)
{
    for (uint32_t i = 0; i < RDMA_REPL_WINDOW; i++) {
        if (st->slots[i].in_use) {
            return 1;
        }
    }
    return 0;
}

static kvs_replica_t *add_replica(int tcp_fd,
                                  struct sockaddr_in *addr,
                                  replica_state_t state)
{
    kvs_replica_t *r = (kvs_replica_t *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }

    r->tcp_fd = tcp_fd;
    memcpy(&r->tcp_addr, addr, sizeof(*addr));
    r->state = state;
    r->rdb_fd = -1;
    pthread_mutex_init(&r->queue_mutex, NULL);

    pthread_mutex_lock(&replicas_mutex);
    r->next = g_replicas;
    g_replicas = r;
    pthread_mutex_unlock(&replicas_mutex);

    return r;
}

static kvs_replica_t *find_replica_by_tcp_fd(int tcp_fd)
{
    kvs_replica_t *r;

    pthread_mutex_lock(&replicas_mutex);
    r = g_replicas;
    while (r) {
        if (r->tcp_fd == tcp_fd) {
            break;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&replicas_mutex);

    return r;
}

static kvs_replica_t *find_replica_by_rdma_cm_id(struct rdma_cm_id *cm_id)
{
    kvs_replica_t *r;

    pthread_mutex_lock(&replicas_mutex);
    r = g_replicas;
    while (r) {
        if (r->rdma_cm_id == cm_id) {
            break;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&replicas_mutex);

    return r;
}

static kvs_replica_t *find_waiting_replica_by_peer(struct rdma_cm_id *cm_id)
{
    struct sockaddr_in *peer;
    kvs_replica_t *r;

    if (!cm_id) {
        return NULL;
    }

    peer = (struct sockaddr_in *)rdma_get_peer_addr(cm_id);

    pthread_mutex_lock(&replicas_mutex);
    r = g_replicas;
    while (r) {
        if (r->state == KVS_REPL_STATE_WAIT_RDMA &&
            r->tcp_addr.sin_addr.s_addr == peer->sin_addr.s_addr) {
            break;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&replicas_mutex);

    return r;
}

static void master_cleanup_conn_state(struct rdma_cm_id *cm_id)
{
    if (!cm_id || !cm_id->context) {
        return;
    }
    conn_manger_t *conn = (conn_manger_t *)cm_id->context;
    master_sync_state_t *st = (master_sync_state_t *)conn->user_data;
    if (st) {
        conn->user_data = NULL;
        master_sync_state_free(st);
    }
}

static void remove_replica(kvs_replica_t *replica)
{
    kvs_replica_t **pp;

    if (!replica) {
        return;
    }

    pthread_mutex_lock(&replicas_mutex);
    pp = &g_replicas;
    while (*pp) {
        if (*pp == replica) {
            *pp = replica->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&replicas_mutex);

    if (replica->rdma_cm_id) {
        struct rdma_cm_id *cm_id = replica->rdma_cm_id;
        replica->rdma_cm_id = NULL;
        rdma_disconnect(cm_id);
        master_cleanup_conn_state(cm_id);
        destroy_connection(cm_id);
    }
    if (replica->tcp_fd >= 0) {
        close(replica->tcp_fd);
    }
    if (replica->cmd_queue) {
        free(replica->cmd_queue);
    }
    if (replica->rdb_fd >= 0) {
        close(replica->rdb_fd);
    }

    replica_free_snapshot(replica);
    pthread_mutex_destroy(&replica->queue_mutex);
    free(replica);
}

static int master_prepare_snapshot(kvs_replica_t *replica, conn_manger_t *conn)
{
    char *buf = NULL;
    size_t len = 0;

    if (!replica || !conn || !conn->pd) {
        return -1;
    }

    replica_free_snapshot(replica);

    if (rdb_save_to_memory(&buf, &len) != 0) {
        fprintf(stderr, "rdb_save_to_memory failed\n");
        return -1;
    }

    replica->rdb_buf = buf;
    replica->rdb_size = (off_t)len;
    replica->rdb_offset = 0;
    replica->rdb_seq = 1;

    return 0;
}

static int master_send_one_locked(master_sync_state_t *st)
{
    kvs_replica_t *replica = st->replica;
    conn_manger_t *conn = st->conn;
    rdma_chunk_header_t *hdr;
    struct ibv_sge sge;
    uint32_t slot;
    size_t remaining = 0;
    size_t max_payload;
    size_t to_send;
    int is_last;
    int total_len;

    if (!replica || !conn || !conn->has_remote_region) {
        return -1;
    }

    if (replica->rdb_size == 0) {
        if (st->empty_sent) {
            return 0;
        }
        to_send = 0;
        is_last = 1;
    } else {
        if (st->next_offset >= replica->rdb_size) {
            return 0;
        }
        remaining = (size_t)(replica->rdb_size - st->next_offset);
        max_payload = conn->remote_slot_size - sizeof(*hdr);
        if (max_payload > RDMA_REPL_CHUNK_SIZE) {
            max_payload = RDMA_REPL_CHUNK_SIZE;
        }
        to_send = remaining < max_payload ? remaining : max_payload;
        is_last = (st->next_offset + (off_t)to_send >= replica->rdb_size);
    }

    slot = (uint32_t)((st->next_seq - 1) % conn->remote_slot_count);
    if (slot >= RDMA_REPL_WINDOW) {
        fprintf(stderr, "remote slot exceeds local window: slot=%u\n", slot);
        return -1;
    }
    if (st->slots[slot].in_use) {
        return 0;
    }

    hdr = (rdma_chunk_header_t *)rdma_send_slot_ptr(conn, slot);
    if (!hdr) {
        return -1;
    }

    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = RDMA_REPL_MAGIC;
    hdr->version = RDMA_REPL_VERSION;
    hdr->seq = st->next_seq;
    hdr->offset = (uint64_t)st->next_offset;
    hdr->total_size = (uint64_t)replica->rdb_size;
    hdr->len = (uint32_t)to_send;
    hdr->is_last = (uint32_t)is_last;
    hdr->slot_id = slot;
    hdr->header_len = (uint32_t)sizeof(*hdr);
    /* 每个 RDMA_WRITE 分片都带 payload 校验和；从机写入 RDB 前先校验，避免静默脏数据落盘。 */
    hdr->checksum = to_send > 0 ?
                    rdma_fnv1a64(replica->rdb_buf + st->next_offset, to_send) :
                    rdma_fnv1a64(NULL, 0);

    if (to_send > 0) {
        if (!replica->rdb_buf) {
            fprintf(stderr, "missing RDB snapshot buffer\n");
            return -1;
        }
        /* Soft-iWARP 对多 SGE RDMA_WRITE_WITH_IMM 更敏感；合并成单个已注册发送槽后再写远端内存。 */
        memcpy((char *)hdr + sizeof(*hdr), replica->rdb_buf + st->next_offset, to_send);
    }

    total_len = (int)(sizeof(*hdr) + to_send);

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)hdr;
    sge.length = (uint32_t)total_len;
    sge.lkey = conn->send_mr->lkey;

    st->slots[slot].in_use = 1;
    st->slots[slot].write_done = 0;
    st->slots[slot].acked = 0;
    st->slots[slot].is_last = is_last;
    st->slots[slot].slot = slot;
    st->slots[slot].seq = st->next_seq;
    st->slots[slot].offset = st->next_offset;
    st->slots[slot].len = to_send;

    if (post_rdma_writev_slot(conn,
                              &sge,
                              1,
                              total_len,
                              slot,
                              st->next_seq,
                              1) != 0) {
        memset(&st->slots[slot], 0, sizeof(st->slots[slot]));
        fprintf(stderr, "post RDMA write failed\n");
        return -1;
    }

    if (replica->rdb_size == 0) {
        st->empty_sent = 1;
    } else {
        st->next_offset += (off_t)to_send;
    }
    st->next_seq++;
    return 1;
}

static int master_pump_locked(master_sync_state_t *st)
{
    int sent_any = 0;

    while (!st->done_received) {
        int ret = master_send_one_locked(st);
        if (ret < 0) {
            rdma_disconnect(st->conn->cm_id);
            return -1;
        }
        if (ret == 0) {
            break;
        }
        sent_any = 1;
    }

    return sent_any;
}

static void master_finish_fullsync(master_sync_state_t *st)
{
    kvs_replica_t *replica;

    if (!st || !st->replica) {
        return;
    }
    replica = st->replica;

    replica_free_snapshot(replica);
    replica->state = KVS_REPL_STATE_WAIT_FINSYNC;

    if (replica->tcp_fd >= 0) {
        send(replica->tcp_fd, "+DONE\r\n", 7, MSG_NOSIGNAL);
    }

    printf("[master] fullsync finished, waiting FINSYNC\n");
}

static int master_should_finish_locked(master_sync_state_t *st)
{
    return st->done_received && !st->finishing && !master_has_inflight_locked(st);
}

void kvs_replication_master_handle_sync(int tcp_fd, struct sockaddr_in *addr)
{
    kvs_replica_t *replica = add_replica(tcp_fd, addr, KVS_REPL_STATE_WAIT_RDMA);
    const char *ok = "+OK\r\n";

    if (!replica) {
        close(tcp_fd);
        return;
    }

    send(tcp_fd, ok, strlen(ok), MSG_NOSIGNAL);
}

void kvs_replication_master_handle_finsync(int tcp_fd)
{
    kvs_replica_t *replica = find_replica_by_tcp_fd(tcp_fd);
    if (!replica) {
        return;
    }

    if (replica->state != KVS_REPL_STATE_WAIT_FINSYNC) {
        return;
    }

    replica->state = KVS_REPL_STATE_ONLINE;
    if (replica->rdma_cm_id) {
        rdma_disconnect(replica->rdma_cm_id);
    }
}

static void master_release_completed_slots_locked(master_sync_state_t *st)
{
    for (uint32_t i = 0; i < RDMA_REPL_WINDOW; i++) {
        if (st->slots[i].in_use && st->slots[i].write_done && st->slots[i].acked) {
            memset(&st->slots[i], 0, sizeof(st->slots[i]));
        }
    }
}

static void on_master_write_complete(struct ibv_wc *wc)
{
    rdma_wr_ctx_t *ctx = rdma_wc_ctx(wc);
    conn_manger_t *conn = rdma_wc_conn(wc);
    master_sync_state_t *st;
    int do_finish = 0;

    if (!conn || !ctx) {
        return;
    }

    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "RDMA write/send error: %s\n", ibv_wc_status_str(wc->status));
        rdma_disconnect(conn->cm_id);
        return;
    }

    conn->completed_send++;

    if (ctx->type != RDMA_WR_CTX_WRITE) {
        return;
    }

    st = (master_sync_state_t *)conn->user_data;
    if (!st) {
        return;
    }

    pthread_mutex_lock(&st->lock);
    if (ctx->slot < RDMA_REPL_WINDOW &&
        st->slots[ctx->slot].in_use &&
        st->slots[ctx->slot].seq == ctx->seq) {
        st->slots[ctx->slot].write_done = 1;
        /* 数据已经通过 RDMA_WRITE 到达从机内存；再用 SEND 通知从机处理对应 data slot。 */
        if (post_ctrl_slot(conn,
                           ctx->slot,
                           RDMA_CTRL_CHUNK,
                           ctx->seq,
                           ctx->slot,
                           0) != 0) {
            rdma_disconnect(conn->cm_id);
        }
    }

    master_release_completed_slots_locked(st);
    master_pump_locked(st);

    if (master_should_finish_locked(st)) {
        st->finishing = 1;
        do_finish = 1;
    }
    pthread_mutex_unlock(&st->lock);

    if (do_finish) {
        master_finish_fullsync(st);
    }
}

static void on_master_recv_complete(struct ibv_wc *wc)
{
    rdma_wr_ctx_t *ctx = rdma_wc_ctx(wc);
    conn_manger_t *conn = rdma_wc_conn(wc);
    master_sync_state_t *st;
    rdma_ctrl_msg_t ctrl_copy;
    rdma_ctrl_msg_t *ctrl;
    int do_finish = 0;

    if (!conn || !ctx) {
        return;
    }

    if (wc->status != IBV_WC_SUCCESS) {
        if (wc->status != IBV_WC_WR_FLUSH_ERR) {
            fprintf(stderr, "RDMA ctrl recv error: %s\n", ibv_wc_status_str(wc->status));
        }
        rdma_disconnect(conn->cm_id);
        return;
    }

    conn->completed_recv++;

    ctrl = (rdma_ctrl_msg_t *)rdma_ctrl_recv_slot_ptr(conn, ctx->slot);
    if (!ctrl) {
        rdma_disconnect(conn->cm_id);
        return;
    }
    memcpy(&ctrl_copy, ctrl, sizeof(ctrl_copy));

    if (post_recv_slot(conn, ctx->slot) != 0) {
        return;
    }

    st = (master_sync_state_t *)conn->user_data;
    if (!st) {
        return;
    }

    if (ctrl_copy.magic != RDMA_REPL_MAGIC) {
        fprintf(stderr, "invalid RDMA ctrl magic\n");
        rdma_disconnect(conn->cm_id);
        return;
    }

    if (ctrl_copy.type == RDMA_CTRL_ERROR) {
        fprintf(stderr, "slave reported RDMA fullsync error: seq=%llu offset=%llu status=%u\n",
                (unsigned long long)ctrl_copy.seq,
                (unsigned long long)ctrl_copy.offset,
                ctrl_copy.status);
        rdma_disconnect(conn->cm_id);
        return;
    }

    if (ctrl_copy.type != RDMA_CTRL_ACK && ctrl_copy.type != RDMA_CTRL_DONE) {
        fprintf(stderr, "unknown RDMA ctrl type: %u\n", ctrl_copy.type);
        rdma_disconnect(conn->cm_id);
        return;
    }

    pthread_mutex_lock(&st->lock);

    for (uint32_t i = 0; i < RDMA_REPL_WINDOW; i++) {
        if (st->slots[i].in_use && st->slots[i].seq <= ctrl_copy.seq) {
            st->slots[i].acked = 1;
        }
    }

    st->replica->rdb_offset = (off_t)ctrl_copy.offset;
    st->replica->rdb_seq = ctrl_copy.seq + 1;

    if (ctrl_copy.type == RDMA_CTRL_DONE) {
        st->done_received = 1;
    }

    master_release_completed_slots_locked(st);
    master_pump_locked(st);

    if (master_should_finish_locked(st)) {
        st->finishing = 1;
        do_finish = 1;
    }
    pthread_mutex_unlock(&st->lock);

    if (do_finish) {
        master_finish_fullsync(st);
    }
}

static void on_master_completion(struct ibv_wc *wc)
{
    rdma_wr_ctx_t *ctx = rdma_wc_ctx(wc);
    if (!ctx) {
        return;
    }

    if (ctx->type == RDMA_WR_CTX_RECV) {
        on_master_recv_complete(wc);
    } else if (ctx->type == RDMA_WR_CTX_WRITE || ctx->type == RDMA_WR_CTX_SEND) {
        on_master_write_complete(wc);
    }
}

static int master_start_fullsync(kvs_replica_t *replica)
{
    conn_manger_t *conn;
    master_sync_state_t *st;

    if (!replica || !replica->rdma_cm_id) {
        return -1;
    }

    conn = (conn_manger_t *)replica->rdma_cm_id->context;
    if (!conn) {
        return -1;
    }

    if (master_prepare_snapshot(replica, conn) != 0) {
        rdma_disconnect(replica->rdma_cm_id);
        return -1;
    }

    st = master_sync_state_new(replica, conn);
    if (!st) {
        replica_free_snapshot(replica);
        rdma_disconnect(replica->rdma_cm_id);
        return -1;
    }
    conn->user_data = st;

    if (post_recv_n(conn, RDMA_REPL_CTRL_RECV_DEPTH) != 0) {
        rdma_disconnect(replica->rdma_cm_id);
        return -1;
    }

    pthread_mutex_lock(&st->lock);
    if (master_pump_locked(st) < 0) {
        pthread_mutex_unlock(&st->lock);
        return -1;
    }
    pthread_mutex_unlock(&st->lock);

    return 0;
}

static void on_rdma_connect_request(struct rdma_cm_event *event)
{
    struct rdma_cm_id *cm_id = event->id;
    struct rdma_conn_param conn_param;
    rdma_peer_region_t region;
    kvs_replica_t *replica;
    conn_manger_t *conn;

    memset(&region, 0, sizeof(region));
    if (event->param.conn.private_data &&
        event->param.conn.private_data_len == sizeof(region)) {
        memcpy(&region, event->param.conn.private_data, sizeof(region));
    } else {
        fprintf(stderr, "RDMA connect request missing peer region\n");
        rdma_reject(cm_id, NULL, 0);
        return;
    }

    replica = find_waiting_replica_by_peer(cm_id);
    if (!replica) {
        fprintf(stderr,
                "RDMA request from %s has no pending SYNC\n",
                get_inet_addr_str(cm_id));
        rdma_reject(cm_id, NULL, 0);
        return;
    }

    if (initialize_connection(cm_id, on_master_completion) != 0) {
        rdma_reject(cm_id, NULL, 0);
        return;
    }

    conn = (conn_manger_t *)cm_id->context;
    conn->user_data = NULL;
    if (rdma_set_remote_region(conn, &region) != 0) {
        rdma_reject(cm_id, NULL, 0);
        destroy_connection(cm_id);
        return;
    }

    replica->rdma_cm_id = cm_id;

    memset(&conn_param, 0, sizeof(conn_param));
    if (rdma_accept(cm_id, &conn_param) != 0) {
        perror("rdma_accept");
        destroy_connection(cm_id);
    }
}

static void on_rdma_connect_established(struct rdma_cm_id *cm_id)
{
    kvs_replica_t *replica = find_replica_by_rdma_cm_id(cm_id);
    if (!replica) {
        rdma_disconnect(cm_id);
        return;
    }

    if (replica->state != KVS_REPL_STATE_WAIT_RDMA) {
        return;
    }

    if (master_start_fullsync(replica) != 0) {
        fprintf(stderr, "master_start_fullsync failed\n");
    }
}

static void on_rdma_disconnected(struct rdma_cm_id *cm_id)
{
    kvs_replica_t *replica = find_replica_by_rdma_cm_id(cm_id);
    if (replica) {
        replica->rdma_cm_id = NULL;
        replica_free_snapshot(replica);
    }

    master_cleanup_conn_state(cm_id);
    destroy_connection(cm_id);
}

static void *rdma_listen_thread_func(void *arg)
{
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listen_id = NULL;
    struct sockaddr_in addr;

    (void)arg;

    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        return NULL;
    }

    int flags = fcntl(ec->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(ec->fd, F_SETFL, flags | O_NONBLOCK);
    }

    if (rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2000);
    inet_pton(AF_INET, g_config.bind_ip, &addr.sin_addr);

    if (rdma_bind_addr(listen_id, (struct sockaddr *)&addr)) {
        perror("rdma_bind_addr");
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    if (rdma_listen(listen_id, 10)) {
        fprintf(stderr, "rdma_listen failed: %s\n", strerror(errno));
        rdma_destroy_id(listen_id);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    printf("RDMA replication listening on %s:%d\n", g_config.bind_ip, 2000);

    while (rdma_running) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = ec->fd;
        pfd.events = POLLIN;

        int pret = poll(&pfd, 1, 100);
        if (pret < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pret == 0) {
            continue;
        }

        while (rdma_running) {
            struct rdma_cm_event *event = NULL;
            if (rdma_get_cm_event(ec, &event) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                continue;
            }

            if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
                on_rdma_connect_request(event);
            } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
                on_rdma_connect_established(event->id);
            } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
                struct rdma_cm_id *id = event->id;
                rdma_ack_cm_event(event);
                on_rdma_disconnected(id);
                continue;
            } else if (event->event == RDMA_CM_EVENT_REJECTED ||
                       event->event == RDMA_CM_EVENT_UNREACHABLE ||
                       event->event == RDMA_CM_EVENT_ADDR_ERROR ||
                       event->event == RDMA_CM_EVENT_ROUTE_ERROR) {
                fprintf(stderr, "RDMA CM event error: %s\n",
                        rdma_event_str(event->event));
            }

            rdma_ack_cm_event(event);
        }
    }

    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    return NULL;
}

int kvs_replication_master_init(void)
{
    g_replicas = NULL;
    rdma_running = 1;

    if (pthread_create(&rdma_listen_thread, NULL, rdma_listen_thread_func, NULL) != 0) {
        perror("pthread_create rdma listener");
        rdma_running = 0;
        return -1;
    }

    rdma_listen_thread_started = 1;
    return 0;
}

void kvs_replication_master_cleanup(void)
{
    kvs_replica_t *r;

    rdma_running = 0;
    if (rdma_listen_thread_started) {
        pthread_join(rdma_listen_thread, NULL);
        rdma_listen_thread_started = 0;
    }

    while (1) {
        pthread_mutex_lock(&replicas_mutex);
        r = g_replicas;
        pthread_mutex_unlock(&replicas_mutex);

        if (!r) {
            break;
        }
        remove_replica(r);
    }
}
