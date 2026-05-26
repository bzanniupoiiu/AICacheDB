#ifndef __XDP_H
#define __XDP_H

#define ETH_P_IP 0x0800

/*
 * 单个 TCP payload 最大拷贝长度。
 * 普通以太网 MTU 下 4096 足够。
 * 如果你的环境是 loopback/GSO，大包可能更大，可以改成 8192 或 16384。
 */
#define MAX_DATA 4096

struct event {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;

    /*
     * full_len 是当前 TCP payload 的原始长度；
     * payload_len 是实际拷贝到 payload[] 的长度。
     * 如果 full_len > payload_len，说明被截断，用户态会丢弃并重置该 flow。
     */
    __u32 full_len;
    __u32 payload_len;

    char payload[MAX_DATA];
};

#endif