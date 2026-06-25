#include <stdint.h>
#include <stdio.h>

#include "../rdma.h"

int main(void)
{
    uint32_t imm;
    const char payload[] = "rdma-write-checksum";

    if (RDMA_REPL_WINDOW < 16u) {
        fprintf(stderr, "RDMA_REPL_WINDOW must keep enough in-flight slots for pipelined full sync\n");
        return 1;
    }

    if (RDMA_REPL_SLOT_SIZE != 1024u * 1024u) {
        fprintf(stderr, "RDMA_REPL_SLOT_SIZE must be 1MB to reduce WR/CQ overhead\n");
        return 1;
    }

    if (RDMA_REPL_SEND_SLOTS < RDMA_REPL_WINDOW) {
        fprintf(stderr, "RDMA_REPL_SEND_SLOTS must cover all pipelined data slots\n");
        return 1;
    }

    if (RDMA_REPL_ACK_EVERY == 0u || RDMA_REPL_ACK_EVERY > RDMA_REPL_WINDOW) {
        fprintf(stderr, "RDMA_REPL_ACK_EVERY must batch ACKs without exceeding the pipeline window\n");
        return 1;
    }

    if (RDMA_REPL_CHUNK_SIZE != RDMA_REPL_SLOT_SIZE - (uint32_t)sizeof(rdma_chunk_header_t)) {
        fprintf(stderr, "RDMA_REPL_CHUNK_SIZE must leave room for chunk header\n");
        return 1;
    }

    imm = rdma_imm_pack(0u, RDMA_REPL_CHUNK_SIZE);
    if (rdma_imm_slot(imm) != 0u || rdma_imm_len(imm) != RDMA_REPL_CHUNK_SIZE) {
        fprintf(stderr, "RDMA immediate slot/length encoding is invalid\n");
        return 1;
    }

    if (rdma_fnv1a64(payload, sizeof(payload) - 1) ==
        rdma_fnv1a64(payload, sizeof(payload) - 2)) {
        fprintf(stderr, "RDMA checksum must change when payload length changes\n");
        return 1;
    }

    return 0;
}
