

#ifndef __RDMA_H__
#define __RDMA_H__

#define BUFFER_SIZE  (8*1024*1024)

typedef struct {
    uint32_t len;
    uint32_t is_last;
} msg_header_t;

typedef struct conn_manger {
    char *recv_buffer;
    char *send_buffer;
    struct ibv_mr *recv_mr;
    struct ibv_mr *send_mr;
    struct ibv_qp *qp;//queue_pair
    struct rdma_cm_id *cm_id;     // 新增：保存 RDMA CM ID

    int running;   // ⭐ 新增
    void *user_data;            // 用户自定义数据（用于关联 kvs_replica_t）
    
} conn_manger_t;

typedef void (*on_completion_t)(struct ibv_wc *wc);//work——complete

typedef struct cq_params {
    struct ibv_comp_channel *channel;
    on_completion_t on_complete;
} cq_params_t;




static char *get_inet_addr_str(struct rdma_cm_id *cm_id) {
    
    struct sockaddr_in *addr_in = (struct sockaddr_in *)rdma_get_peer_addr(cm_id);
    return inet_ntoa(addr_in->sin_addr);
}

// wc --> 
static void *cq_poller(void *arg) {
    struct ibv_wc wc[10];                 // 改为数组
    struct ibv_cq *cq;
    void *ctx = NULL;
    cq_params_t *params = (cq_params_t*)arg;
    struct ibv_comp_channel *channel = params->channel;
    on_completion_t on_complete = params->on_complete;
    free(params);

    while (1) {

        if (ibv_get_cq_event(channel, &cq, &ctx) != 0)
            continue;

        conn_manger_t *conn = (conn_manger_t *)ctx;

        ibv_ack_cq_events(cq, 1);
        ibv_req_notify_cq(cq, 0);

        if (!conn || !conn->running) {
            break;   // ⭐ 退出线程
        }

        int n;
        while ((n = ibv_poll_cq(cq, 10, wc)) > 0) {
            for (int i = 0; i < n; i++) {
                on_complete(&wc[i]);
            }
        }
    }
    return NULL;

}



static void destory_connection(struct rdma_cm_id *cm_id) {

  

     conn_manger_t *conn = (conn_manger_t *)cm_id->context;
    if (!conn) return;

    // ⭐ 1. 先让 CQ 线程停
    conn->running = 0;

    // ⭐ 2. 等一会，让 poller 退出（关键）
    usleep(1000 * 2);  // 2ms

    // ⭐ 3. 再销毁 QP（避免 flush error）
    if (cm_id->qp)
        rdma_destroy_qp(cm_id);

    // ⭐ 4. 再释放 MR
    if (conn->recv_mr)
        ibv_dereg_mr(conn->recv_mr);
    if (conn->send_mr)
        ibv_dereg_mr(conn->send_mr);

    // ⭐ 5. 再释放 buffer
    free(conn->recv_buffer);
    free(conn->send_buffer);

    rdma_destroy_id(cm_id);

    free(conn);

}

static void initialize_connection(struct rdma_cm_id *cm_id, on_completion_t on_complete) {


    // 1️⃣ 分配 PD
    struct ibv_pd *pd = ibv_alloc_pd(cm_id->verbs);
    if (!pd) {
        perror("ibv_alloc_pd failed");
        exit(-1);
    }

    // 2️⃣ 创建 completion channel
    struct ibv_comp_channel *channel = ibv_create_comp_channel(cm_id->verbs);
    if (!channel) {
        perror("ibv_create_comp_channel failed");
        exit(-1);
    }

    // 3️⃣ 分配 conn_manger（必须清零！）
    conn_manger_t *conn_manger = (conn_manger_t *)malloc(sizeof(conn_manger_t));
    memset(conn_manger, 0, sizeof(*conn_manger));

    conn_manger->cm_id = cm_id;
    conn_manger->running = 1;

    conn_manger->user_data = NULL;  // 新增


    // ⚠️ 提前绑定 context（给 CQ 用）
    cm_id->context = conn_manger;

    // 4️⃣ 创建 CQ（绑定 conn_manger）
    struct ibv_cq *cq = ibv_create_cq(cm_id->verbs, 1024, conn_manger, channel, 0);
    if (!cq) {
        perror("ibv_create_cq failed");
        exit(-1);
    }

    if (ibv_req_notify_cq(cq, 0)) {
        perror("ibv_req_notify_cq failed");
        exit(-1);
    }

    // 5️⃣ 启动 CQ poller 线程
    cq_params_t *params = malloc(sizeof(cq_params_t));
    params->channel = channel;
    params->on_complete = on_complete;

    pthread_t cq_poller_thread;
    pthread_create(&cq_poller_thread, NULL, cq_poller, params);
    pthread_detach(cq_poller_thread);

    // 6️⃣ 创建 QP
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));

    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_RC;

    qp_attr.cap.max_send_wr = 1024;
    qp_attr.cap.max_recv_wr = 1024;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(cm_id, pd, &qp_attr)) {
        perror("rdma_create_qp failed");
        exit(-1);
    }

    // ✅ QP 创建完再赋值
    conn_manger->qp = cm_id->qp;

    // 7️⃣ 分配 buffer
    conn_manger->recv_buffer = (char *)malloc(BUFFER_SIZE);
    conn_manger->send_buffer = (char *)malloc(BUFFER_SIZE);

    if (!conn_manger->recv_buffer || !conn_manger->send_buffer) {
        perror("malloc buffer failed");
        exit(-1);
    }

    // 8️⃣ 注册 MR
    conn_manger->recv_mr = ibv_reg_mr(pd,
                                     conn_manger->recv_buffer,
                                     BUFFER_SIZE,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    conn_manger->send_mr = ibv_reg_mr(pd,
                                     conn_manger->send_buffer,
                                     BUFFER_SIZE,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);

    if (!conn_manger->recv_mr || !conn_manger->send_mr) {
        perror("ibv_reg_mr failed");
        exit(-1);
    }

}

// copy data to conn_manager->send_buffer
static int post_send(conn_manger_t *conn_manger, int length) {

    char *sbuffer = conn_manger->send_buffer;

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)sbuffer;
    sge.length = length;
    sge.lkey = conn_manger->send_mr->lkey; // Assume lkey is set appropriately

    struct ibv_send_wr send_wr, *bad_send_wr = NULL;
    memset(&send_wr, 0, sizeof(send_wr));
    send_wr.wr_id = (uintptr_t)conn_manger;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;    

    if (ibv_post_send(conn_manger->qp, &send_wr, &bad_send_wr)) {
        perror("ibv_post_send failed");
        rdma_disconnect(conn_manger->cm_id);
        return -1;
    }
    return 0;

}

// copy data from conn_manager->recv_buffer
static int post_recv(conn_manger_t *conn_manger) {

    char *rbuffer = conn_manger->recv_buffer;
    memset(rbuffer, 0, BUFFER_SIZE);

    struct ibv_sge sge;
    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)rbuffer;
    sge.length = BUFFER_SIZE;
    sge.lkey = conn_manger->recv_mr->lkey; // Assume lkey is set appropriately

    struct ibv_recv_wr recv_wr, *bad_recv_wr = NULL;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = (uintptr_t)conn_manger;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;    

    if (ibv_post_recv(conn_manger->qp, &recv_wr, &bad_recv_wr)) {
        perror("ibv_post_recv failed");
        // 可以断开连接或退出
        rdma_disconnect(conn_manger->cm_id);
        return -1;
    }
    return 0;

}


#endif


