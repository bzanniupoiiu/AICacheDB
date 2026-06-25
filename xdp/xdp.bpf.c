#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include "xdp.h"

char LICENSE[] SEC("license") = "GPL";

const volatile __u16 target_port = 8888;

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24);
} events SEC(".maps");

static __always_inline int bounds_ok(void *ptr, void *end, int size)
{
    return ptr + size <= end;
}

SEC("xdp")
int xdp_filter(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if (!bounds_ok(eth, data_end, sizeof(*eth)))
        return XDP_PASS;

    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *ip = (void *)(eth + 1);
    if (!bounds_ok(ip, data_end, sizeof(*ip)))
        return XDP_PASS;

    if (ip->protocol != IPPROTO_TCP)
        return XDP_PASS;

    int ip_len = ip->ihl * 4;
    if (ip_len < (int)sizeof(*ip))
        return XDP_PASS;

    struct tcphdr *tcp = (void *)ip + ip_len;
    if (!bounds_ok(tcp, data_end, sizeof(*tcp)))
        return XDP_PASS;

    if (target_port != 0 && tcp->dest != bpf_htons(target_port))
        return XDP_PASS;

    int tcp_len = tcp->doff * 4;
    if (tcp_len < (int)sizeof(*tcp))
        return XDP_PASS;

    void *payload = (void *)tcp + tcp_len;
    if (payload >= data_end)
        return XDP_PASS;

    int full_len = data_end - payload;
    if (full_len <= 0)
        return XDP_PASS;

    int copy_len = full_len;
    if (copy_len > MAX_DATA)
        copy_len = MAX_DATA;

    struct event *e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return XDP_PASS;

    e->saddr = ip->saddr;
    e->daddr = ip->daddr;
    e->sport = tcp->source;
    e->dport = tcp->dest;
    e->seq = bpf_ntohl(tcp->seq);
    e->full_len = full_len;
    e->payload_len = copy_len;

    bpf_probe_read_kernel(e->payload, copy_len, payload);

    bpf_ringbuf_submit(e, 0);

    return XDP_PASS;
}
