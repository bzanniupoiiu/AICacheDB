#ifndef __RDMA_H__
#define __RDMA_H__

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef IBV_WC_RECV_RDMA_WITH_IMM
#define IBV_WC_RECV_RDMA_WITH_IMM IBV_WC_RECV
#endif

#define RDMA_REPL_MAGIC          0x52444d41u
#define RDMA_REPL_VERSION        3u

#define RDMA_REPL_WINDOW         64u
#define RDMA_REPL_SLOT_SIZE      (1024u * 1024u)
#define RDMA_REPL_RING_SIZE      (RDMA_REPL_WINDOW * RDMA_REPL_SLOT_SIZE)

#define RDMA_REPL_CTRL_RECV_DEPTH 128u
#define RDMA_REPL_SEND_SLOTS      64u
#define RDMA_REPL_SEND_SLOT_SIZE  RDMA_REPL_SLOT_SIZE
#define RDMA_REPL_CTRL_SLOT_SIZE  256u

#define BUFFER_SIZE RDMA_REPL_SLOT_SIZE

#define RDMA_POST_SEND_RETRY_MAX 500
#define RDMA_POST_SEND_RETRY_US  2000
#define RDMA_REPL_ACK_EVERY      16u

#define RDMA_IMM_SLOT_BITS       8u
#define RDMA_IMM_LEN_MASK        0x00ffffffu
#define RDMA_REPL_CHUNK_SIZE     (RDMA_REPL_SLOT_SIZE - (uint32_t)sizeof(rdma_chunk_header_t))

typedef enum {
    RDMA_CTRL_ACK = 1,
    RDMA_CTRL_DONE = 2,
    RDMA_CTRL_ERROR = 3,
    RDMA_CTRL_CHUNK = 4
} rdma_ctrl_type_t;

typedef struct {
    uint64_t addr;
    uint32_t rkey;
    uint32_t length;
    uint32_t slot_size;
    uint32_t slot_count;
    uint32_t version;
    uint32_t flags;
} rdma_peer_region_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t seq;
    uint64_t offset;
    uint64_t total_size;
    uint64_t checksum;
    uint32_t len;
    uint32_t is_last;
    uint32_t slot_id;
    uint32_t header_len;
} rdma_chunk_header_t;

typedef struct {
    uint32_t magic;
    uint32_t type;
    uint64_t seq;
    uint64_t offset;
    uint32_t status;
    uint32_t reserved;
} rdma_ctrl_msg_t;

typedef struct conn_manger conn_manger_t;

typedef enum {
    RDMA_WR_CTX_RECV = 1,
    RDMA_WR_CTX_SEND = 2,
    RDMA_WR_CTX_WRITE = 3
} rdma_wr_ctx_type_t;

typedef struct {
    conn_manger_t *conn;
    uint32_t type;
    uint32_t slot;
    uint64_t seq;
} rdma_wr_ctx_t;

struct conn_manger {
    char *recv_buffer;
    size_t recv_buffer_len;

    char *send_buffer;
    size_t send_buffer_len;

    char *ctrl_recv_buffer;
    size_t ctrl_recv_buffer_len;

    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *channel;
    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;
    struct ibv_mr *ctrl_recv_mr;
    struct ibv_qp *qp;
    struct rdma_cm_id *cm_id;

    uint64_t remote_addr;
    uint32_t remote_rkey;
    uint32_t remote_len;
    uint32_t remote_slot_size;
    uint32_t remote_slot_count;
    int has_remote_region;

    uint32_t local_slot_size;
    uint32_t local_slot_count;

    rdma_wr_ctx_t recv_ctx[RDMA_REPL_CTRL_RECV_DEPTH];
    rdma_wr_ctx_t send_ctx[RDMA_REPL_SEND_SLOTS];

    uint64_t posted_send;
    uint64_t completed_send;
    uint64_t posted_recv;
    uint64_t completed_recv;

    int running;
    int done;
    int failed;
    int cq_thread_started;
    int destroying;
    pthread_t cq_thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    void *user_data;
};

typedef void (*on_completion_t)(struct ibv_wc *wc);

typedef struct cq_params {
    conn_manger_t *conn;
    on_completion_t on_complete;
} cq_params_t;

static int rdma_post_errno_retryable(int err)
{
    return err == EAGAIN || err == ENOMEM || err == ENOSPC || err == EBUSY;
}

static uint64_t rdma_fnv1a64_update(uint64_t h, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t rdma_fnv1a64(const void *data, size_t len)
{
    return rdma_fnv1a64_update(1469598103934665603ull, data, len);
}

static char *get_inet_addr_str(struct rdma_cm_id *cm_id)
{
    struct sockaddr_in *addr_in = (struct sockaddr_in *)rdma_get_peer_addr(cm_id);
    return inet_ntoa(addr_in->sin_addr);
}

static inline uint32_t rdma_imm_pack(uint32_t slot, uint32_t len)
{
    return ((slot & 0xffu) << 24) | (len & RDMA_IMM_LEN_MASK);
}

static inline uint32_t rdma_imm_slot(uint32_t imm)
{
    return (imm >> 24) & 0xffu;
}

static inline uint32_t rdma_imm_len(uint32_t imm)
{
    return imm & RDMA_IMM_LEN_MASK;
}

static inline char *rdma_data_slot_ptr(conn_manger_t *conn, uint32_t slot)
{
    if (!conn || !conn->recv_buffer || slot >= conn->local_slot_count) {
        return NULL;
    }
    return conn->recv_buffer + ((size_t)slot * conn->local_slot_size);
}

static inline char *rdma_send_slot_ptr(conn_manger_t *conn, uint32_t slot)
{
    if (!conn || !conn->send_buffer) {
        return NULL;
    }
    slot %= RDMA_REPL_SEND_SLOTS;
    return conn->send_buffer + ((size_t)slot * RDMA_REPL_SEND_SLOT_SIZE);
}

static inline char *rdma_ctrl_recv_slot_ptr(conn_manger_t *conn, uint32_t slot)
{
    if (!conn || !conn->ctrl_recv_buffer) {
        return NULL;
    }
    slot %= RDMA_REPL_CTRL_RECV_DEPTH;
    return conn->ctrl_recv_buffer + ((size_t)slot * RDMA_REPL_CTRL_SLOT_SIZE);
}

static inline uint64_t rdma_remote_slot_addr(conn_manger_t *conn, uint32_t slot)
{
    return conn->remote_addr + ((uint64_t)slot * conn->remote_slot_size);
}

static inline rdma_wr_ctx_t *rdma_wc_ctx(struct ibv_wc *wc)
{
    if (!wc || wc->wr_id == 0) {
        return NULL;
    }
    return (rdma_wr_ctx_t *)(uintptr_t)wc->wr_id;
}

static inline conn_manger_t *rdma_wc_conn(struct ibv_wc *wc)
{
    rdma_wr_ctx_t *ctx = rdma_wc_ctx(wc);
    return ctx ? ctx->conn : NULL;
}

static inline uint32_t rdma_wc_data_slot(struct ibv_wc *wc, rdma_wr_ctx_t *ctx)
{
#ifdef IBV_WC_WITH_IMM
    if (wc && (wc->wc_flags & IBV_WC_WITH_IMM)) {
        return rdma_imm_slot(ntohl(wc->imm_data));
    }
#endif
    return ctx ? ctx->slot : 0;
}

static void rdma_mark_done(conn_manger_t *conn)
{
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->lock);
    conn->done = 1;
    pthread_cond_broadcast(&conn->cond);
    pthread_mutex_unlock(&conn->lock);
}

static void rdma_mark_failed(conn_manger_t *conn)
{
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->lock);
    conn->failed = 1;
    conn->running = 0;
    pthread_cond_broadcast(&conn->cond);
    pthread_mutex_unlock(&conn->lock);
}

static void rdma_wait_done(conn_manger_t *conn)
{
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->lock);
    while (conn->running && !conn->done && !conn->failed) {
        pthread_cond_wait(&conn->cond, &conn->lock);
    }
    pthread_mutex_unlock(&conn->lock);
}

static int rdma_set_remote_region(conn_manger_t *conn,
                                  const rdma_peer_region_t *region)
{
    if (!conn || !region) {
        return -1;
    }
    if (region->version != RDMA_REPL_VERSION) {
        fprintf(stderr, "RDMA version mismatch: local=%u remote=%u\n",
                RDMA_REPL_VERSION, region->version);
        return -1;
    }
    if (region->slot_count == 0 || region->slot_size <= sizeof(rdma_chunk_header_t)) {
        fprintf(stderr, "invalid remote RDMA region: slot_count=%u slot_size=%u\n",
                region->slot_count, region->slot_size);
        return -1;
    }
    if (region->length < region->slot_count * region->slot_size) {
        fprintf(stderr, "invalid remote RDMA region length=%u slot_count=%u slot_size=%u\n",
                region->length, region->slot_count, region->slot_size);
        return -1;
    }

    conn->remote_addr = region->addr;
    conn->remote_rkey = region->rkey;
    conn->remote_len = region->length;
    conn->remote_slot_size = region->slot_size;
    conn->remote_slot_count = region->slot_count;
    conn->has_remote_region = 1;
    return 0;
}

static int rdma_get_local_region(conn_manger_t *conn,
                                 rdma_peer_region_t *region)
{
    if (!conn || !region || !conn->recv_mr || !conn->recv_buffer) {
        return -1;
    }

    memset(region, 0, sizeof(*region));
    region->addr = (uint64_t)(uintptr_t)conn->recv_buffer;
    region->rkey = conn->recv_mr->rkey;
    region->length = (uint32_t)conn->recv_buffer_len;
    region->slot_size = conn->local_slot_size;
    region->slot_count = conn->local_slot_count;
    region->version = RDMA_REPL_VERSION;
    return 0;
}

static void *cq_poller(void *arg)
{
    struct ibv_wc wc[16];
    cq_params_t *params = (cq_params_t *)arg;
    conn_manger_t *conn = params->conn;
    on_completion_t on_complete = params->on_complete;
    free(params);

    while (conn->running) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = conn->channel->fd;
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

        while (conn->running) {
            struct ibv_cq *cq = NULL;
            void *ctx = NULL;
            if (ibv_get_cq_event(conn->channel, &cq, &ctx) != 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                continue;
            }

            ibv_ack_cq_events(cq, 1);
            ibv_req_notify_cq(cq, 0);

            int n;
            while ((n = ibv_poll_cq(cq, 16, wc)) > 0) {
                for (int i = 0; i < n; i++) {
                    on_complete(&wc[i]);
                }
            }
        }
    }

    pthread_mutex_lock(&conn->lock);
    pthread_cond_broadcast(&conn->cond);
    pthread_mutex_unlock(&conn->lock);
    return NULL;
}

static int post_recv_slot(conn_manger_t *conn, uint32_t slot)
{
    if (!conn || !conn->qp || !conn->ctrl_recv_mr || !conn->ctrl_recv_buffer) {
        return -1;
    }
    if (slot >= RDMA_REPL_CTRL_RECV_DEPTH) {
        return -1;
    }

    char *buf = rdma_ctrl_recv_slot_ptr(conn, slot);
    if (!buf) {
        return -1;
    }
    memset(buf, 0, RDMA_REPL_CTRL_SLOT_SIZE);

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = RDMA_REPL_CTRL_SLOT_SIZE;
    sge.lkey = conn->ctrl_recv_mr->lkey;

    conn->recv_ctx[slot].conn = conn;
    conn->recv_ctx[slot].type = RDMA_WR_CTX_RECV;
    conn->recv_ctx[slot].slot = slot;
    conn->recv_ctx[slot].seq = 0;

    struct ibv_recv_wr recv_wr;
    struct ibv_recv_wr *bad_recv_wr = NULL;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = (uintptr_t)&conn->recv_ctx[slot];
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    if (ibv_post_recv(conn->qp, &recv_wr, &bad_recv_wr)) {
        perror("ibv_post_recv failed");
        rdma_disconnect(conn->cm_id);
        return -1;
    }

    conn->posted_recv++;
    return 0;
}

static int post_recv_n(conn_manger_t *conn, uint32_t n)
{
    if (!conn) {
        return -1;
    }
    if (n > RDMA_REPL_CTRL_RECV_DEPTH) {
        n = RDMA_REPL_CTRL_RECV_DEPTH;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (post_recv_slot(conn, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int post_send_slot(conn_manger_t *conn,
                          uint32_t slot,
                          int length,
                          uint64_t seq)
{
    if (!conn || !conn->qp || !conn->send_mr || length <= 0) {
        return -1;
    }

    slot %= RDMA_REPL_SEND_SLOTS;
    char *buf = rdma_send_slot_ptr(conn, slot);
    if (!buf || length > (int)RDMA_REPL_SEND_SLOT_SIZE) {
        return -1;
    }

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)buf;
    sge.length = (uint32_t)length;
    sge.lkey = conn->send_mr->lkey;

    conn->send_ctx[slot].conn = conn;
    conn->send_ctx[slot].type = RDMA_WR_CTX_SEND;
    conn->send_ctx[slot].slot = slot;
    conn->send_ctx[slot].seq = seq;

    struct ibv_send_wr send_wr;
    struct ibv_send_wr *bad_send_wr = NULL;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = (uintptr_t)&conn->send_ctx[slot];
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;

    for (int retry = 0; retry < RDMA_POST_SEND_RETRY_MAX; retry++) {
        int ret;
        bad_send_wr = NULL;
        ret = ibv_post_send(conn->qp, &send_wr, &bad_send_wr);
        if (ret == 0) {
            conn->posted_send++;
            return 0;
        }

        errno = ret > 0 ? ret : errno;
        if (rdma_post_errno_retryable(errno)) {
            usleep(RDMA_POST_SEND_RETRY_US);
            continue;
        }

        perror("ibv_post_send failed");
        break;
    }

    if (rdma_post_errno_retryable(errno)) {
        perror("ibv_post_send retry exhausted");
    }
    rdma_disconnect(conn->cm_id);
    return -1;
}

static int post_ctrl_slot(conn_manger_t *conn,
                          uint32_t slot,
                          rdma_ctrl_type_t type,
                          uint64_t seq,
                          uint64_t offset,
                          uint32_t status)
{
    if (!conn) {
        return -1;
    }

    char *buf = rdma_send_slot_ptr(conn, slot);
    if (!buf) {
        return -1;
    }

    rdma_ctrl_msg_t *ctrl = (rdma_ctrl_msg_t *)buf;
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->magic = RDMA_REPL_MAGIC;
    ctrl->type = (uint32_t)type;
    ctrl->seq = seq;
    ctrl->offset = offset;
    ctrl->status = status;

    return post_send_slot(conn, slot, (int)sizeof(*ctrl), seq);
}

static int post_rdma_writev_slot(conn_manger_t *conn,
                                 struct ibv_sge *sges,
                                 int num_sge,
                                 int total_length,
                                 uint32_t remote_slot,
                                 uint64_t seq,
                                 int signaled)
{
    if (!conn || !conn->qp || !conn->has_remote_region ||
        !sges || num_sge <= 0 || total_length <= 0) {
        return -1;
    }
    if (remote_slot >= conn->remote_slot_count) {
        fprintf(stderr, "invalid remote RDMA slot=%u slot_count=%u\n",
                remote_slot, conn->remote_slot_count);
        rdma_disconnect(conn->cm_id);
        return -1;
    }
    if ((uint32_t)total_length > conn->remote_slot_size) {
        fprintf(stderr, "RDMA remote slot too small: need=%d slot_size=%u\n",
                total_length, conn->remote_slot_size);
        rdma_disconnect(conn->cm_id);
        return -1;
    }
    if ((uint32_t)total_length > RDMA_IMM_LEN_MASK) {
        fprintf(stderr, "RDMA immediate length overflow: %d\n", total_length);
        rdma_disconnect(conn->cm_id);
        return -1;
    }

    uint32_t send_slot = remote_slot % RDMA_REPL_SEND_SLOTS;
    conn->send_ctx[send_slot].conn = conn;
    conn->send_ctx[send_slot].type = RDMA_WR_CTX_WRITE;
    conn->send_ctx[send_slot].slot = remote_slot;
    conn->send_ctx[send_slot].seq = seq;

    struct ibv_send_wr wr;
    struct ibv_send_wr *bad_wr = NULL;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = (uintptr_t)&conn->send_ctx[send_slot];
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    wr.sg_list = sges;
    wr.num_sge = num_sge;
    wr.wr.rdma.remote_addr = rdma_remote_slot_addr(conn, remote_slot);
    wr.wr.rdma.rkey = conn->remote_rkey;

    for (int retry = 0; retry < RDMA_POST_SEND_RETRY_MAX; retry++) {
        int ret;
        bad_wr = NULL;
        ret = ibv_post_send(conn->qp, &wr, &bad_wr);
        if (ret == 0) {
            conn->posted_send++;
            return 0;
        }

        errno = ret > 0 ? ret : errno;
        if (rdma_post_errno_retryable(errno)) {
            usleep(RDMA_POST_SEND_RETRY_US);
            continue;
        }

        perror("ibv_post_send RDMA_WRITE failed");
        break;
    }

    if (rdma_post_errno_retryable(errno)) {
        perror("ibv_post_send RDMA_WRITE retry exhausted");
    }
    rdma_disconnect(conn->cm_id);
    return -1;
}

static int rdma_alloc_aligned(char **out, size_t len)
{
    void *p = NULL;
    int ret = posix_memalign(&p, 4096, len);
    if (ret != 0) {
        errno = ret;
        perror("posix_memalign RDMA buffer");
        return -1;
    }
    memset(p, 0, len);
    *out = (char *)p;
    return 0;
}

static int initialize_connection(struct rdma_cm_id *cm_id, on_completion_t on_complete)
{
    int ret = -1;
    conn_manger_t *conn = NULL;
    cq_params_t *params = NULL;

    if (!cm_id || !on_complete) {
        return -1;
    }

    conn = (conn_manger_t *)calloc(1, sizeof(*conn));
    if (!conn) {
        perror("calloc conn");
        return -1;
    }

    pthread_mutex_init(&conn->lock, NULL);
    pthread_cond_init(&conn->cond, NULL);
    conn->cm_id = cm_id;
    conn->running = 1;
    conn->local_slot_size = RDMA_REPL_SLOT_SIZE;
    conn->local_slot_count = RDMA_REPL_WINDOW;
    conn->recv_buffer_len = RDMA_REPL_RING_SIZE;
    conn->send_buffer_len = RDMA_REPL_SEND_SLOTS * RDMA_REPL_SEND_SLOT_SIZE;
    conn->ctrl_recv_buffer_len = RDMA_REPL_CTRL_RECV_DEPTH * RDMA_REPL_CTRL_SLOT_SIZE;
    cm_id->context = conn;

    conn->pd = ibv_alloc_pd(cm_id->verbs);
    if (!conn->pd) {
        perror("ibv_alloc_pd failed");
        goto fail;
    }

    conn->channel = ibv_create_comp_channel(cm_id->verbs);
    if (!conn->channel) {
        perror("ibv_create_comp_channel failed");
        goto fail;
    }

    int flags = fcntl(conn->channel->fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(conn->channel->fd, F_SETFL, flags | O_NONBLOCK);
    }

    conn->cq = ibv_create_cq(cm_id->verbs, 8192, conn, conn->channel, 0);
    if (!conn->cq) {
        perror("ibv_create_cq failed");
        goto fail;
    }

    if (ibv_req_notify_cq(conn->cq, 0)) {
        perror("ibv_req_notify_cq failed");
        goto fail;
    }

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = conn->cq;
    qp_attr.recv_cq = conn->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 8192;
    qp_attr.cap.max_recv_wr = 8192;
    qp_attr.cap.max_send_sge = 2;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(cm_id, conn->pd, &qp_attr)) {
        perror("rdma_create_qp failed");
        goto fail;
    }
    conn->qp = cm_id->qp;

    if (rdma_alloc_aligned(&conn->recv_buffer, conn->recv_buffer_len) != 0 ||
        rdma_alloc_aligned(&conn->send_buffer, conn->send_buffer_len) != 0 ||
        rdma_alloc_aligned(&conn->ctrl_recv_buffer, conn->ctrl_recv_buffer_len) != 0) {
        goto fail;
    }

    conn->recv_mr = ibv_reg_mr(conn->pd,
                               conn->recv_buffer,
                               conn->recv_buffer_len,
                               IBV_ACCESS_LOCAL_WRITE |
                               IBV_ACCESS_REMOTE_WRITE);
    conn->send_mr = ibv_reg_mr(conn->pd,
                               conn->send_buffer,
                               conn->send_buffer_len,
                               IBV_ACCESS_LOCAL_WRITE);
    conn->ctrl_recv_mr = ibv_reg_mr(conn->pd,
                                    conn->ctrl_recv_buffer,
                                    conn->ctrl_recv_buffer_len,
                                    IBV_ACCESS_LOCAL_WRITE);
    if (!conn->recv_mr || !conn->send_mr || !conn->ctrl_recv_mr) {
        perror("ibv_reg_mr failed");
        goto fail;
    }

    params = (cq_params_t *)malloc(sizeof(*params));
    if (!params) {
        perror("malloc cq params");
        goto fail;
    }
    params->conn = conn;
    params->on_complete = on_complete;

    if (pthread_create(&conn->cq_thread, NULL, cq_poller, params) != 0) {
        perror("pthread_create cq_poller");
        free(params);
        goto fail;
    }
    conn->cq_thread_started = 1;

    return 0;

fail:
    if (conn) {
        conn->running = 0;
        if (conn->qp) {
            rdma_destroy_qp(cm_id);
            conn->qp = NULL;
        }
        if (conn->recv_mr) ibv_dereg_mr(conn->recv_mr);
        if (conn->send_mr) ibv_dereg_mr(conn->send_mr);
        if (conn->ctrl_recv_mr) ibv_dereg_mr(conn->ctrl_recv_mr);
        if (conn->cq) ibv_destroy_cq(conn->cq);
        if (conn->channel) ibv_destroy_comp_channel(conn->channel);
        if (conn->pd) ibv_dealloc_pd(conn->pd);
        free(conn->recv_buffer);
        free(conn->send_buffer);
        free(conn->ctrl_recv_buffer);
        pthread_cond_destroy(&conn->cond);
        pthread_mutex_destroy(&conn->lock);
        free(conn);
        cm_id->context = NULL;
    }
    return ret;
}

static void destroy_connection(struct rdma_cm_id *cm_id)
{
    conn_manger_t *conn;

    if (!cm_id) {
        return;
    }

    conn = (conn_manger_t *)cm_id->context;
    if (!conn) {
        rdma_destroy_id(cm_id);
        return;
    }

    pthread_mutex_lock(&conn->lock);
    if (conn->destroying) {
        pthread_mutex_unlock(&conn->lock);
        return;
    }
    conn->destroying = 1;
    conn->running = 0;
    pthread_cond_broadcast(&conn->cond);
    pthread_mutex_unlock(&conn->lock);

    if (conn->cq_thread_started) {
        pthread_join(conn->cq_thread, NULL);
        conn->cq_thread_started = 0;
    }

    if (cm_id->qp) {
        rdma_destroy_qp(cm_id);
        conn->qp = NULL;
    }

    if (conn->recv_mr) ibv_dereg_mr(conn->recv_mr);
    if (conn->send_mr) ibv_dereg_mr(conn->send_mr);
    if (conn->ctrl_recv_mr) ibv_dereg_mr(conn->ctrl_recv_mr);
    if (conn->cq) ibv_destroy_cq(conn->cq);
    if (conn->channel) ibv_destroy_comp_channel(conn->channel);
    if (conn->pd) ibv_dealloc_pd(conn->pd);

    free(conn->recv_buffer);
    free(conn->send_buffer);
    free(conn->ctrl_recv_buffer);

    pthread_cond_destroy(&conn->cond);
    pthread_mutex_destroy(&conn->lock);

    cm_id->context = NULL;
    rdma_destroy_id(cm_id);
    free(conn);
}

#endif
