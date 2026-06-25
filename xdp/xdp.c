#include <arpa/inet.h>
#include <bpf/libbpf.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "xdp.skel.h"
#include "xdp.h"

#define MAX_CMD_LEN       (8 * 1024 * 1024)
#define WAL_MAX_BYTES     (512ULL * 1024ULL * 1024ULL)
#define WAL_RING_CAP      65536
#define FLOW_MAX          1024
#define FLOW_BUF_MAX      (8 * 1024 * 1024)
#define FLOW_IDLE_SEC     60
#define MAX_ATTACH_IFS    8

typedef struct {
    char slave_ip[64];
    int slave_port;
    int capture_port;
    char ifaces[256];
    int connect_timeout_ms;
    int ack_timeout_ms;
} forwarder_config_t;

typedef struct wal_entry {
    uint64_t seq;
    int len;
    char *data;
} wal_entry_t;

typedef struct {
    wal_entry_t *ring;
    int head;
    int tail;
    int cap;
    uint64_t next_seq;
    uint64_t last_acked_seq;
    size_t bytes;
    int count;
} wal_queue_t;

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
    int have_seq;
    uint32_t next_seq;
    time_t last_seen;
};

static volatile int running = 1;
static int slave_fd = -1;
static int syncing = 0;
static wal_queue_t wal;
static struct flow_state flows[FLOW_MAX];
static int attached_ifindexes[MAX_ATTACH_IFS];
static int attached_cnt = 0;

static forwarder_config_t cfg = {
    .slave_ip = "192.168.32.132",
    .slave_port = 8000,
    .capture_port = 8888,
    .ifaces = "lo,ens33",
    .connect_timeout_ms = 3000,
    .ack_timeout_ms = 3000,
};

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--slave-ip IP] [--slave-port PORT] "
            "[--capture-port PORT] [--ifaces lo,ens33] "
            "[--connect-timeout-ms N] [--ack-timeout-ms N]\n",
            prog);
}

static int parse_int_arg(const char *s, int min, int max, int *out)
{
    char *end = NULL;
    long v;

    if (!s || !out) {
        return -1;
    }

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > max) {
        return -1;
    }

    *out = (int)v;
    return 0;
}

static int parse_args(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 1;
        } else if (strcmp(argv[i], "--slave-ip") == 0 && i + 1 < argc) {
            snprintf(cfg.slave_ip, sizeof(cfg.slave_ip), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--slave-port") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 1, 65535, &cfg.slave_port) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--capture-port") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 0, 65535, &cfg.capture_port) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ifaces") == 0 && i + 1 < argc) {
            snprintf(cfg.ifaces, sizeof(cfg.ifaces), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--connect-timeout-ms") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 100, 600000, &cfg.connect_timeout_ms) != 0) {
                return -1;
            }
        } else if (strcmp(argv[i], "--ack-timeout-ms") == 0 && i + 1 < argc) {
            if (parse_int_arg(argv[++i], 100, 600000, &cfg.ack_timeout_ms) != 0) {
                return -1;
            }
        } else {
            usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

static char *trim_space(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s)) {
        s++;
    }

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }

    return s;
}

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

static int tcp_seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static void reset_flow_state(struct flow_state *st)
{
    if (!st) {
        return;
    }
    st->len = 0;
    st->have_seq = 0;
    st->next_seq = 0;
    st->last_seen = time(NULL);
}

static void free_flow_state(struct flow_state *st)
{
    if (!st || !st->used) {
        return;
    }

    free(st->buf);
    memset(st, 0, sizeof(*st));
}

static void cleanup_idle_flows(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < FLOW_MAX; i++) {
        if (flows[i].used && now - flows[i].last_seen > FLOW_IDLE_SEC) {
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

static int normalize_payload_by_seq(struct flow_state *st,
                                    const struct event *e,
                                    const char **payload,
                                    uint32_t *payload_len)
{
    uint32_t end_seq;

    if (!st || !e || !payload || !payload_len) {
        return -1;
    }

    if (!st->have_seq) {
        st->have_seq = 1;
        st->next_seq = e->seq + e->full_len;
        return 0;
    }

    if (e->seq == st->next_seq) {
        st->next_seq += e->full_len;
        return 0;
    }

    end_seq = e->seq + e->full_len;
    if (tcp_seq_before(e->seq, st->next_seq)) {
        uint32_t overlap = st->next_seq - e->seq;

        if (overlap >= e->full_len) {
            return 1;
        }

        *payload += overlap;
        *payload_len -= overlap;
        st->next_seq += *payload_len;
        return 0;
    }

    fprintf(stderr,
            "TCP out-of-order gap on flow, seq=%u expected=%u end=%u; reset flow\n",
            e->seq,
            st->next_seq,
            end_seq);
    reset_flow_state(st);
    return -1;
}

static void wal_clear(void)
{
    if (wal.ring) {
        for (int i = 0; i < wal.cap; i++) {
            free(wal.ring[i].data);
            memset(&wal.ring[i], 0, sizeof(wal.ring[i]));
        }
    }

    wal.head = 0;
    wal.tail = 0;
    wal.bytes = 0;
    wal.count = 0;
    wal.last_acked_seq = 0;
    wal.next_seq = 1;
}

static int wal_init(void)
{
    memset(&wal, 0, sizeof(wal));
    wal.cap = WAL_RING_CAP;
    wal.ring = (wal_entry_t *)calloc((size_t)wal.cap, sizeof(wal_entry_t));
    if (!wal.ring) {
        fprintf(stderr, "calloc WAL ring failed\n");
        return -1;
    }
    wal.next_seq = 1;
    return 0;
}

static void wal_destroy(void)
{
    wal_clear();
    free(wal.ring);
    memset(&wal, 0, sizeof(wal));
}

static int wal_push(const char *data, int len)
{
    wal_entry_t *e;

    if (!data || len <= 0) {
        return 0;
    }

    if (len > MAX_CMD_LEN) {
        fprintf(stderr, "command too large for WAL: %d\n", len);
        return -1;
    }

    if (wal.bytes + (size_t)len > WAL_MAX_BYTES) {
        fprintf(stderr,
                "WAL memory limit reached: bytes=%zu incoming=%d limit=%llu\n",
                wal.bytes,
                len,
                (unsigned long long)WAL_MAX_BYTES);
        return -1;
    }

    if (!wal.ring || wal.count >= wal.cap) {
        fprintf(stderr,
                "WAL ring full: count=%d cap=%d bytes=%zu\n",
                wal.count,
                wal.cap,
                wal.bytes);
        return -1;
    }

    e = &wal.ring[wal.tail];
    e->data = (char *)malloc((size_t)len);
    if (!e->data) {
        return -1;
    }

    memcpy(e->data, data, (size_t)len);
    e->len = len;
    e->seq = wal.next_seq++;

    /* fullsync 期间写命令只进入环形队列；FINSYNC 后按 head->tail 顺序发送。 */
    wal.tail = (wal.tail + 1) % wal.cap;
    wal.bytes += (size_t)len;
    wal.count++;

    return 0;
}

static void wal_pop_head(void)
{
    wal_entry_t *e;

    if (!wal.ring || wal.count <= 0) {
        return;
    }

    e = &wal.ring[wal.head];

    wal.bytes -= (size_t)e->len;
    wal.count--;
    wal.last_acked_seq = e->seq;

    free(e->data);
    memset(e, 0, sizeof(*e));
    wal.head = (wal.head + 1) % wal.cap;
}

static int send_raw(int fd, const char *data, int len)
{
    int sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, (size_t)(len - sent), MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (int)n;
    }

    return 0;
}

static int wait_slave_ack(void)
{
    char buf[64];
    int len = 0;

    while (len < (int)sizeof(buf) - 1) {
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = slave_fd;
        pfd.events = POLLIN;

        int pret = poll(&pfd, 1, cfg.ack_timeout_ms);
        if (pret <= 0) {
            return -1;
        }

        ssize_t n = recv(slave_fd, buf + len, sizeof(buf) - 1 - len, 0);
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
        if (strstr(buf, "+ACK\r\n") != NULL) {
            return 0;
        }
    }

    return -1;
}

static void close_slave_conn(void)
{
    if (slave_fd >= 0) {
        close(slave_fd);
        slave_fd = -1;
    }
}

static int connect_slave(void)
{
    struct timeval start;

    if (slave_fd >= 0) {
        return 0;
    }

    gettimeofday(&start, NULL);

    while (running) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("socket slave");
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(cfg.slave_port);

        if (inet_pton(AF_INET, cfg.slave_ip, &addr.sin_addr) <= 0) {
            perror("inet_pton slave_ip");
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            slave_fd = fd;
            printf("[repl] connected to slave %s:%d\n",
                   cfg.slave_ip,
                   cfg.slave_port);
            return 0;
        }

        close(fd);

        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                          (now.tv_usec - start.tv_usec) / 1000;
        if (elapsed_ms >= cfg.connect_timeout_ms) {
            fprintf(stderr,
                    "[repl] connect slave %s:%d timeout after %d ms\n",
                    cfg.slave_ip,
                    cfg.slave_port,
                    cfg.connect_timeout_ms);
            return -1;
        }

        usleep(100 * 1000);
    }

    return -1;
}

static int flush_wal(void)
{
    while (running && !syncing && wal.count > 0) {
        wal_entry_t *head = &wal.ring[wal.head];

        if (connect_slave() != 0) {
            return -1;
        }

        if (send_raw(slave_fd, head->data, head->len) != 0 ||
            wait_slave_ack() != 0) {
            fprintf(stderr,
                    "[repl] send/ack failed for seq=%llu, keep WAL and reconnect\n",
                    (unsigned long long)head->seq);
            close_slave_conn();
            return -1;
        }

        wal_pop_head();
    }

    return 0;
}

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

static int resp_command_len(const char *buf, int len)
{
    int pos = 0;
    int argc = 0;
    int ret;

    if (len <= 0) {
        return 0;
    }
    if (buf[pos] != '*') {
        return -1;
    }

    pos++;
    ret = read_number_crlf(buf, len, &pos, &argc);
    if (ret <= 0) {
        return ret;
    }
    if (argc <= 0 || argc > 1024) {
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        int bulk_len = 0;

        if (pos >= len) {
            return 0;
        }
        if (buf[pos] != '$') {
            return -1;
        }
        pos++;

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

static int get_command_name(const char *payload, int len, char *out, int out_len)
{
    const char *end = payload + len;
    const char *p;
    int bulk_len = 0;
    int copy_len;

    if (!payload || len < 4 || payload[0] != '*') {
        return -1;
    }

    p = payload + 1;
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

    while (p < end && *p >= '0' && *p <= '9') {
        bulk_len = bulk_len * 10 + (*p - '0');
        p++;
    }
    if (p + 2 > end || p[0] != '\r' || p[1] != '\n') {
        return -1;
    }
    p += 2;

    if (bulk_len <= 0 || p + bulk_len + 2 > end) {
        return -1;
    }

    copy_len = bulk_len < out_len - 1 ? bulk_len : out_len - 1;
    memcpy(out, p, (size_t)copy_len);
    out[copy_len] = '\0';
    return 0;
}

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

static int process_full_command(const char *payload,
                                int payload_len,
                                uint32_t saddr,
                                uint16_t sport)
{
    char cmd_name[64];

    if (get_command_name(payload, payload_len, cmd_name, sizeof(cmd_name)) != 0) {
        fprintf(stderr, "failed to parse RESP command name\n");
        return 0;
    }

    if (strcasecmp(cmd_name, "SYNC") == 0) {
        char ipbuf[INET_ADDRSTRLEN];
        format_ipv4(saddr, ipbuf, sizeof(ipbuf));
        printf("[ctrl] SYNC from %s:%d, start WAL\n", ipbuf, ntohs(sport));

        wal_clear();
        syncing = 1;
        connect_slave();
        return 0;
    }

    if (strcasecmp(cmd_name, "FINSYNC") == 0) {
        printf("[ctrl] FINSYNC, flush WAL count=%d bytes=%zu\n",
               wal.count,
               wal.bytes);
        syncing = 0;
        flush_wal();
        return 0;
    }

    if (!is_write_command(cmd_name)) {
        return 0;
    }

    if (wal_push(payload, payload_len) != 0) {
        fprintf(stderr, "fatal: cannot append command to WAL, stop forwarder\n");
        running = 0;
        return -1;
    }

    if (!syncing) {
        flush_wal();
    }

    return 0;
}

static int handle_event(void *ctx, void *data, size_t len)
{
    struct event *e = (struct event *)data;
    struct flow_state *st;
    const char *payload;
    uint32_t payload_len;
    int seq_ret;

    (void)ctx;
    (void)len;

    if (!e || e->payload_len == 0) {
        return 0;
    }

    if (e->payload_len > MAX_DATA) {
        fprintf(stderr, "invalid payload_len: %u\n", e->payload_len);
        return 0;
    }

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

        st = get_flow_state(e);
        if (st) {
            reset_flow_state(st);
        }
        return 0;
    }

    st = get_flow_state(e);
    if (!st) {
        return 0;
    }

    payload = e->payload;
    payload_len = e->payload_len;
    seq_ret = normalize_payload_by_seq(st, e, &payload, &payload_len);
    if (seq_ret != 0) {
        return 0;
    }

    if (payload_len == 0) {
        return 0;
    }

    if (st->len + (int)payload_len > FLOW_BUF_MAX) {
        fprintf(stderr, "flow buffer overflow, reset flow\n");
        reset_flow_state(st);
        return 0;
    }

    memcpy(st->buf + st->len, payload, payload_len);
    st->len += (int)payload_len;
    st->last_seen = time(NULL);

    while (st->len > 0) {
        int cmd_len = resp_command_len(st->buf, st->len);

        if (cmd_len == 0) {
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

        process_full_command(st->buf, cmd_len, e->saddr, e->sport);

        int remain = st->len - cmd_len;
        if (remain > 0) {
            memmove(st->buf, st->buf + cmd_len, (size_t)remain);
        }
        st->len = remain;
    }

    return 0;
}

static void detach_all(void)
{
    for (int i = 0; i < attached_cnt; i++) {
        bpf_xdp_detach(attached_ifindexes[i], XDP_FLAGS_SKB_MODE, NULL);
    }
    attached_cnt = 0;
}

static int attach_ifaces(struct xdp_bpf *skel)
{
    char ifbuf[sizeof(cfg.ifaces)];
    char *save = NULL;
    char *tok;

    snprintf(ifbuf, sizeof(ifbuf), "%s", cfg.ifaces);

    tok = strtok_r(ifbuf, ",", &save);
    while (tok) {
        char *name = trim_space(tok);
        int ifindex;

        if (*name == '\0') {
            tok = strtok_r(NULL, ",", &save);
            continue;
        }

        ifindex = if_nametoindex(name);
        if (ifindex == 0) {
            fprintf(stderr, "if_nametoindex failed for %s\n", name);
            tok = strtok_r(NULL, ",", &save);
            continue;
        }

        if (bpf_xdp_attach(ifindex,
                           bpf_program__fd(skel->progs.xdp_filter),
                           XDP_FLAGS_SKB_MODE,
                           NULL) != 0) {
            fprintf(stderr, "failed to attach XDP to %s\n", name);
            tok = strtok_r(NULL, ",", &save);
            continue;
        }

        if (attached_cnt < MAX_ATTACH_IFS) {
            attached_ifindexes[attached_cnt++] = ifindex;
        }

        printf("[+] attached XDP to %s\n", name);
        tok = strtok_r(NULL, ",", &save);
    }

    return attached_cnt > 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    struct xdp_bpf *skel = NULL;
    struct ring_buffer *rb = NULL;
    int loop_count = 0;
    int arg_ret;
    int ret = 1;

    arg_ret = parse_args(argc, argv);
    if (arg_ret > 0) {
        return 0;
    }
    if (arg_ret < 0) {
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    memset(flows, 0, sizeof(flows));
    if (wal_init() != 0) {
        return 1;
    }

    skel = xdp_bpf__open();
    if (!skel) {
        fprintf(stderr, "failed to open BPF skeleton\n");
        return 1;
    }

    skel->rodata->target_port = (uint16_t)cfg.capture_port;

    if (xdp_bpf__load(skel) != 0) {
        fprintf(stderr, "failed to load BPF skeleton\n");
        goto cleanup;
    }

    if (attach_ifaces(skel) != 0) {
        fprintf(stderr, "no XDP interface attached\n");
        goto cleanup;
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "failed to create ring buffer\n");
        goto cleanup;
    }

    printf("[*] XDP forwarder running: capture_port=%d slave=%s:%d ifaces=%s\n",
           cfg.capture_port,
           cfg.slave_ip,
           cfg.slave_port,
           cfg.ifaces);

    while (running) {
        int poll_ret = ring_buffer__poll(rb, 100);
        if (poll_ret < 0 && poll_ret != -EINTR) {
            fprintf(stderr, "ring_buffer__poll error: %d\n", poll_ret);
            break;
        }

        if (++loop_count >= 50) {
            cleanup_idle_flows();
            if (!syncing && wal.count > 0) {
                flush_wal();
            }
            loop_count = 0;
        }
    }

    ret = 0;

cleanup:
    printf("[*] cleaning up XDP forwarder...\n");

    if (rb) {
        ring_buffer__free(rb);
    }
    detach_all();
    if (skel) {
        xdp_bpf__destroy(skel);
    }

    close_slave_conn();
    wal_destroy();
    free_all_flows();

    return ret;
}
