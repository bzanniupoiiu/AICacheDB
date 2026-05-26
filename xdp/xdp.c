#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <bpf/libbpf.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <sys/time.h>

#include "xdp.skel.h"
#include "xdp.h"

#define SLAVE_IP   "192.168.32.135"
#define SLAVE_PORT 8000

static volatile int running = 1;

/*
 * WAL 队列。
 * 原来的写法是：
 *   MAX_WAL * MAX_CMD_LEN
 * 会占用非常大的静态内存。
 *
 * 这里改成动态分配，每条命令按实际长度 malloc。
 */
#define MAX_WAL      8192
#define MAX_CMD_LEN  (8 * 1024 * 1024)

struct wal_entry {
    int len;
    char *data;
};

struct wal_queue {
    struct wal_entry buf[MAX_WAL];
    int head;
    int tail;
};

static struct wal_queue wal;

/*
 * 每条 TCP 连接一个 flow buffer。
 * 解决长 RESP 命令被拆成多个 TCP payload 的问题。
 */
#define FLOW_MAX       1024
#define FLOW_BUF_MAX   (8 * 1024 * 1024)
#define FLOW_IDLE_SEC  60

struct flow_key {
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
};

struct flow_state {
    int used;
    struct flow_key key;
    char *buf;
    int len;
    time_t last_seen;
};

static struct flow_state flows[FLOW_MAX];

/* 从机连接 */
static int slave_fd = -1;

/*
 * syncing = 1：主从全量同步期间，写命令先进入 WAL；
 * syncing = 0：直接转发写命令。
 */
static int syncing = 0;

/* 记录 XDP attach 的网卡，退出时 detach */
#define MAX_ATTACH_IFS 8
static int attached_ifindexes[MAX_ATTACH_IFS];
static int attached_cnt = 0;

/* ===================== 信号 ===================== */

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ===================== 工具函数 ===================== */

static void format_ipv4(uint32_t addr, char *buf, size_t len)
{
    struct in_addr in;
    in.s_addr = addr;
    inet_ntop(AF_INET, &in, buf, len);
}

static int flow_key_equal(const struct flow_key *a, const struct flow_key *b)
{
    return a->saddr == b->saddr &&
           a->daddr == b->daddr &&
           a->sport == b->sport &&
           a->dport == b->dport;
}

static void reset_flow_state(struct flow_state *st)
{
    if (!st) return;
    st->len = 0;
    st->last_seen = time(NULL);
}

static void free_flow_state(struct flow_state *st)
{
    if (!st || !st->used) return;

    if (st->buf) {
        free(st->buf);
        st->buf = NULL;
    }

    st->len = 0;
    st->used = 0;
    memset(&st->key, 0, sizeof(st->key));
}

static void cleanup_idle_flows(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < FLOW_MAX; i++) {
        if (!flows[i].used) {
            continue;
        }

        if (now - flows[i].last_seen > FLOW_IDLE_SEC) {
            free_flow_state(&flows[i]);
        }
    }
}

static void free_all_flows(void)
{
    for (int i = 0; i < FLOW_MAX; i++) {
        free_flow_state(&flows[i]);
    }
}

static struct flow_state *get_flow_state(const struct event *e)
{
    struct flow_key key;
    key.saddr = e->saddr;
    key.daddr = e->daddr;
    key.sport = e->sport;
    key.dport = e->dport;

    for (int i = 0; i < FLOW_MAX; i++) {
        if (flows[i].used && flow_key_equal(&flows[i].key, &key)) {
            flows[i].last_seen = time(NULL);
            return &flows[i];
        }
    }

    for (int i = 0; i < FLOW_MAX; i++) {
        if (!flows[i].used) {
            flows[i].buf = (char *)malloc(FLOW_BUF_MAX);
            if (!flows[i].buf) {
                fprintf(stderr, "malloc flow buffer failed\n");
                return NULL;
            }

            flows[i].used = 1;
            flows[i].key = key;
            flows[i].len = 0;
            flows[i].last_seen = time(NULL);
            return &flows[i];
        }
    }

    fprintf(stderr, "flow table full\n");
    return NULL;
}

/* ===================== WAL ===================== */

static void wal_clear(void)
{
    while (wal.head != wal.tail) {
        struct wal_entry *e = &wal.buf[wal.head];

        if (e->data) {
            free(e->data);
            e->data = NULL;
        }

        e->len = 0;
        wal.head = (wal.head + 1) % MAX_WAL;
    }

    wal.head = 0;
    wal.tail = 0;
}

static void wal_push(const char *data, int len)
{
    if (!data || len <= 0) {
        return;
    }

    if (len > MAX_CMD_LEN) {
        fprintf(stderr, "WAL command too large: %d > %d, dropping\n", len, MAX_CMD_LEN);
        return;
    }

    int next = (wal.tail + 1) % MAX_WAL;
    if (next == wal.head) {
        fprintf(stderr, "WAL full, dropping command\n");
        return;
    }

    char *copy = (char *)malloc(len);
    if (!copy) {
        fprintf(stderr, "malloc WAL entry failed\n");
        return;
    }

    memcpy(copy, data, len);

    struct wal_entry *e = &wal.buf[wal.tail];
    e->len = len;
    e->data = copy;
    wal.tail = next;
}

static int wal_count(void)
{
    return (wal.tail - wal.head + MAX_WAL) % MAX_WAL;
}

/* ===================== 从机连接和发送 ===================== */

static int send_to_slave(const char *data, int len)
{
    if (slave_fd < 0) {
        return -1;
    }

    int sent = 0;

    while (sent < len) {
        int n = send(slave_fd, data + sent, len - sent, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }

            fprintf(stderr, "send_to_slave failed: %s\n", strerror(errno));
            close(slave_fd);
            slave_fd = -1;
            return -1;
        }

        if (n == 0) {
            fprintf(stderr, "send_to_slave returned 0\n");
            close(slave_fd);
            slave_fd = -1;
            return -1;
        }

        sent += n;
    }

    return 0;
}

static int connect_slave(void)
{
    if (slave_fd >= 0) {
        close(slave_fd);
        slave_fd = -1;
    }

    struct timeval start;
    gettimeofday(&start, NULL);

    const int timeout_ms = 1000;
    const int retry_interval_ms = 100;

    while (1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket");
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_port = htons(SLAVE_PORT);

        if (inet_pton(AF_INET, SLAVE_IP, &addr.sin_addr) <= 0) {
            perror("inet_pton");
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            slave_fd = fd;
            printf("[SYNC] connected to slave %s:%d\n", SLAVE_IP, SLAVE_PORT);
            return 0;
        }

        close(fd);

        struct timeval now;
        gettimeofday(&now, NULL);

        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_usec - start.tv_usec) / 1000;

        if (elapsed_ms >= timeout_ms) {
            fprintf(stderr,
                    "[SYNC] failed to connect to slave %s:%d within %d ms\n",
                    SLAVE_IP,
                    SLAVE_PORT,
                    timeout_ms);
            return -1;
        }

        usleep(retry_interval_ms * 1000);
    }
}

/* ===================== RESP 解析 ===================== */

/*
 * 从 buf[*pos] 开始解析十进制数字，并要求后面跟 \r\n。
 *
 * 返回值：
 *  1：成功
 *  0：数据不完整
 * -1：协议错误
 */
static int read_number_crlf(const char *buf, int len, int *pos, int *out)
{
    int p = *pos;
    long v = 0;
    int has_digit = 0;

    while (p < len && buf[p] >= '0' && buf[p] <= '9') {
        has_digit = 1;
        v = v * 10 + (buf[p] - '0');

        if (v > FLOW_BUF_MAX) {
            return -1;
        }

        p++;
    }

    if (!has_digit) {
        return -1;
    }

    if (p + 1 >= len) {
        return 0;
    }

    if (buf[p] != '\r' || buf[p + 1] != '\n') {
        return -1;
    }

    *out = (int)v;
    *pos = p + 2;
    return 1;
}

/*
 * 判断 buf[0:len] 是否包含一条完整 RESP Array 命令。
 *
 * 返回值：
 * >0：完整命令长度
 *  0：命令还不完整
 * -1：RESP 格式错误
 */
static int resp_command_len(const char *buf, int len)
{
    int pos = 0;

    if (len <= 0) {
        return 0;
    }

    if (buf[pos] != '*') {
        return -1;
    }

    pos++;

    int argc = 0;
    int ret = read_number_crlf(buf, len, &pos, &argc);
    if (ret <= 0) {
        return ret;
    }

    if (argc <= 0 || argc > 1024) {
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        if (pos >= len) {
            return 0;
        }

        if (buf[pos] != '$') {
            return -1;
        }

        pos++;

        int bulk_len = 0;
        ret = read_number_crlf(buf, len, &pos, &bulk_len);
        if (ret <= 0) {
            return ret;
        }

        if (bulk_len < 0 || bulk_len > FLOW_BUF_MAX) {
            return -1;
        }

        if (pos + bulk_len + 2 > len) {
            return 0;
        }

        pos += bulk_len;

        if (buf[pos] != '\r' || buf[pos + 1] != '\n') {
            return -1;
        }

        pos += 2;
    }

    return pos;
}

/*
 * 解析 RESP 命令的第一个参数，即命令名。
 * 输入必须是一条完整 RESP 命令。
 */
static int get_command_name(const char *payload, int len, char *out, int out_len)
{
    if (!payload || len < 4 || payload[0] != '*') {
        return -1;
    }

    const char *end = payload + len;
    const char *p = payload + 1;

    while (p < end && *p >= '0' && *p <= '9') {
        p++;
    }

    if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
        return -1;
    }

    p += 2;

    if (p >= end || *p != '$') {
        return -1;
    }

    p++;

    int bulk_len = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        bulk_len = bulk_len * 10 + (*p - '0');
        p++;
    }

    if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
        return -1;
    }

    p += 2;

    if (p + bulk_len + 2 > end) {
        return -1;
    }

    int copy_len = bulk_len < out_len - 1 ? bulk_len : out_len - 1;
    memcpy(out, p, copy_len);
    out[copy_len] = '\0';

    return 0;
}

/* ===================== 命令判断和处理 ===================== */

static int is_write_command(const char *cmd)
{
    const char *write_cmds[] = {
        "SET", "RSET", "HSET", "SKSET",
        "DEL", "RDEL", "HDEL", "SKDEL",
        "MOD", "RMOD", "HMOD", "SKMOD",
        "EXPIRE", "REXPIRE", "HEXPIRE", "SKEXPIRE",
        "SETEX", "RSETEX", "HSETEX", "SKSETEX",
        "VSET",
        NULL
    };

    for (int i = 0; write_cmds[i]; i++) {
        if (strcasecmp(cmd, write_cmds[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * 这里处理的是一条完整 RESP 命令。
 * 注意：不要对单个 TCP payload 调这个函数。
 */
static int process_full_command(const char *payload, int payload_len, uint32_t saddr, uint16_t sport)
{
    char cmd_name[64];

    if (get_command_name(payload, payload_len, cmd_name, sizeof(cmd_name)) != 0) {
        fprintf(stderr, "failed to get command name from full RESP command\n");
        return 0;
    }

    if (strcasecmp(cmd_name, "SYNC") == 0) {
        char ipbuf[INET_ADDRSTRLEN];
        format_ipv4(saddr, ipbuf, sizeof(ipbuf));

        printf("[CTRL] SYNC from %s:%d\n", ipbuf, ntohs(sport));

        if (connect_slave() == 0) {
            wal_clear();
            syncing = 1;
        }

        return 0;
    }

    if (strcasecmp(cmd_name, "FINSYNC") == 0) {
        printf("[CTRL] FINSYNC, flushing WAL (%d commands)\n", wal_count());

        while (wal.head != wal.tail) {
            struct wal_entry *we = &wal.buf[wal.head];

            if (we->data && we->len > 0) {
                if (send_to_slave(we->data, we->len) != 0) {
                    fprintf(stderr, "failed to send command during FINSYNC\n");
                    break;
                }
            }

            if (we->data) {
                free(we->data);
                we->data = NULL;
            }

            we->len = 0;
            wal.head = (wal.head + 1) % MAX_WAL;
        }

        syncing = 0;
        return 0;
    }

    if (!is_write_command(cmd_name)) {
        return 0;
    }

    if (syncing) {
        wal_push(payload, payload_len);
    } else if (slave_fd >= 0) {
        if (send_to_slave(payload, payload_len) != 0) {
            fprintf(stderr, "send write command to slave failed: %s\n", cmd_name);
        }
    } else {
        fprintf(stderr, "no slave connection, dropping command: %s\n", cmd_name);
    }

    return 0;
}

/* ===================== ringbuf 回调 ===================== */

static int handle_event(void *ctx, void *data, size_t len)
{
    (void)ctx;
    (void)len;

    struct event *e = (struct event *)data;
    if (!e || e->payload_len == 0) {
        return 0;
    }

    if (e->payload_len > MAX_DATA) {
        fprintf(stderr, "invalid payload_len: %u\n", e->payload_len);
        return 0;
    }

    /*
     * 如果 full_len > payload_len，说明 BPF 端发生截断。
     * 这时继续拼流一定会造成 RESP 错位，所以必须重置该 flow。
     */
    if (e->full_len > e->payload_len) {
        char src[INET_ADDRSTRLEN];
        char dst[INET_ADDRSTRLEN];

        format_ipv4(e->saddr, src, sizeof(src));
        format_ipv4(e->daddr, dst, sizeof(dst));

        fprintf(stderr,
                "payload truncated: %s:%d -> %s:%d full=%u copied=%u, reset flow\n",
                src,
                ntohs(e->sport),
                dst,
                ntohs(e->dport),
                e->full_len,
                e->payload_len);

        struct flow_state *st = get_flow_state(e);
        if (st) {
            reset_flow_state(st);
        }

        return 0;
    }

    struct flow_state *st = get_flow_state(e);
    if (!st) {
        fprintf(stderr, "no flow state available\n");
        return 0;
    }

    if (st->len + (int)e->payload_len > FLOW_BUF_MAX) {
        fprintf(stderr, "flow buffer overflow, reset flow\n");
        reset_flow_state(st);
        return 0;
    }

    /*
     * 1. 把当前 TCP payload 追加到对应连接的 buffer。
     */
    memcpy(st->buf + st->len, e->payload, e->payload_len);
    st->len += e->payload_len;
    st->last_seen = time(NULL);

    /*
     * 2. 循环解析完整 RESP 命令。
     * 一个 TCP payload 可能包含多条命令。
     */
    while (st->len > 0) {
        int cmd_len = resp_command_len(st->buf, st->len);

        if (cmd_len == 0) {
            /*
             * 命令还不完整，等待后续 TCP payload。
             */
            break;
        }

        if (cmd_len < 0) {
            char src[INET_ADDRSTRLEN];
            char dst[INET_ADDRSTRLEN];

            format_ipv4(st->key.saddr, src, sizeof(src));
            format_ipv4(st->key.daddr, dst, sizeof(dst));

            fprintf(stderr,
                    "RESP parse error on flow %s:%d -> %s:%d, reset flow\n",
                    src,
                    ntohs(st->key.sport),
                    dst,
                    ntohs(st->key.dport));

            reset_flow_state(st);
            break;
        }

        /*
         * 3. 只把完整 RESP 命令交给业务处理。
         */
        process_full_command(st->buf, cmd_len, e->saddr, e->sport);

        /*
         * 4. 删除已处理的命令，保留剩余数据。
         */
        int remain = st->len - cmd_len;
        if (remain > 0) {
            memmove(st->buf, st->buf + cmd_len, remain);
        }

        st->len = remain;
    }

    return 0;
}

/* ===================== main ===================== */

int main(void)
{
    struct xdp_bpf *skel = NULL;
    struct ring_buffer *rb = NULL;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    memset(&wal, 0, sizeof(wal));
    memset(flows, 0, sizeof(flows));

    skel = xdp_bpf__open_and_load();
    if (!skel) {
        fprintf(stderr, "failed to open/load BPF skeleton\n");
        return 1;
    }

    const char *ifs[] = {"lo", "ens33"};

    for (int i = 0; i < 2; i++) {
        int ifindex = if_nametoindex(ifs[i]);
        if (ifindex == 0) {
            fprintf(stderr, "if_nametoindex failed for %s\n", ifs[i]);
            continue;
        }

        if (bpf_xdp_attach(ifindex,
                           bpf_program__fd(skel->progs.xdp_filter),
                           XDP_FLAGS_SKB_MODE,
                           NULL) != 0) {
            fprintf(stderr, "failed to attach XDP to %s\n", ifs[i]);
            continue;
        }

        if (attached_cnt < MAX_ATTACH_IFS) {
            attached_ifindexes[attached_cnt++] = ifindex;
        }

        printf("[+] attached XDP to %s\n", ifs[i]);
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "failed to create ring buffer\n");
        goto cleanup;
    }

    printf("[*] XDP forwarder running, waiting for commands...\n");

    int loop_count = 0;

    while (running) {
        int ret = ring_buffer__poll(rb, 100);
        if (ret < 0 && ret != -EINTR) {
            fprintf(stderr, "ring_buffer__poll error: %d\n", ret);
            break;
        }

        /*
         * 定期清理长时间没有新数据的 flow。
         */
        if (++loop_count >= 50) {
            cleanup_idle_flows();
            loop_count = 0;
        }
    }

cleanup:
    printf("[*] cleaning up...\n");

    if (rb) {
        ring_buffer__free(rb);
    }

    for (int i = 0; i < attached_cnt; i++) {
        bpf_xdp_detach(attached_ifindexes[i], XDP_FLAGS_SKB_MODE, NULL);
    }

    if (skel) {
        xdp_bpf__destroy(skel);
    }

    if (slave_fd >= 0) {
        close(slave_fd);
        slave_fd = -1;
    }

    wal_clear();
    free_all_flows();

    return 0;
}