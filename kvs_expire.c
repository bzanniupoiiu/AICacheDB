// kvs_expire.c
#include "kvstore.h"
#include <pthread.h>
#include <unistd.h>

// 外部声明存储引擎全局变量（已在 kvstore.c 中定义）
#if ENABLE_ARRAY
extern kvs_array_t global_array;
#endif
#if ENABLE_RBTREE
extern kvs_rbtree_t global_rbtree;
#endif
#if ENABLE_HASH
extern kvs_hash_t global_hash;
#endif
#if ENABLE_SKIPLIST
extern kvs_skiplist_t global_skiplist;
#endif

static pthread_t expire_scanner_thread;
static int expire_scanner_running = 0;

void kvs_expire_scan(void) {
#if ENABLE_ARRAY
    kvs_array_scan_expired(&global_array);
#endif
#if ENABLE_RBTREE
    kvs_rbtree_scan_expired(&global_rbtree);
#endif
#if ENABLE_HASH
    kvs_hash_scan_expired(&global_hash);
#endif
#if ENABLE_SKIPLIST
    kvs_skiplist_scan_expired(&global_skiplist);
#endif
}

static void *expire_scanner_loop(void *arg) {
    while (expire_scanner_running) {
        usleep(100 * 1000);  // 100ms
        kvs_expire_scan();
    }
    return NULL;
}

int kvs_start_expire_scanner(void) {
    if (expire_scanner_running) return 0;
    expire_scanner_running = 1;
    if (pthread_create(&expire_scanner_thread, NULL, expire_scanner_loop, NULL) != 0) {
        expire_scanner_running = 0;
        return -1;
    }
    return 0;
}

void kvs_stop_expire_scanner(void) {
    if (!expire_scanner_running) return;
    expire_scanner_running = 0;
    pthread_join(expire_scanner_thread, NULL);
}