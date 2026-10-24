#ifndef AIC_DAILY_H
#define AIC_DAILY_H
#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <signal.h>
#define AIC_VERSION "0.3.0"
#define MAX_REQUEST (1024U * 1024U)
#define MAX_OUTPUT (4U * 1024U * 1024U)
#define MAX_ARGS 16
#define MAX_DIM 4096
#define MAX_TOPK 100
#define BUCKETS 8192
typedef struct { const unsigned char *p; size_t n; } Slice;
typedef struct { unsigned char *p; size_t n, cap; int error; } Buffer;
typedef struct Entry {
    struct Entry *next;
    size_t kn, vn;
    int64_t expires;
    uint32_t dim;
    unsigned char *key, *value;
    float *vector;
} Entry;
typedef struct {
    Entry *buckets[BUCKETS];
    size_t bytes, count, limit;
    int fd, lockfd, dirfd, failed, persistent;
    uint64_t wal_bytes, writes;
    char path[4096];
    uint64_t hash_seed;
} Store;
typedef struct {
    char bind[64], dir[4096], password[256];
    unsigned port, maxclients, idle;
    size_t maxmemory;
    int persistent, check;
} Config;
extern volatile sig_atomic_t stopping;
int64_t now_ms(void);
void buf_add(Buffer *, const void *, size_t);
void buf_printf(Buffer *, const char *, ...);
void bulk(Buffer *, const void *, size_t);
void reply_error(Buffer *, const char *);
void buf_free(Buffer *);
int parse_resp(const unsigned char *, size_t, Slice *, int *, size_t *);
int parse_int(Slice, int64_t *);
int parse_vector(Slice, unsigned, float **);
int store_open(Store *, const Config *);
void store_close(Store *);
Entry *store_get(Store *, Slice);
int store_put(Store *, Slice, Slice, int64_t, unsigned, const float *);
int store_del(Store *, Slice);
int store_compact(Store *);
int store_backup(Store *);
void store_sweep(Store *);
int run_server(Store *, const Config *);
#endif
