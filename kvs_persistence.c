#define _GNU_SOURCE
#include "kvstore.h"
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <liburing.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
// 提供 C11 原子操作。
// 这里用于 AOF 环形队列的 write_idx、read_idx、io_error 等变量

#define AOF_URING_ENTRIES 8192  //iouring的总槽位
#define AOF_IO_DEPTH 64         //同时飞行的请求数,也就是最多允许 64 个 AOF 写请求同时提交给内核但尚未完成。
#define AOF_BATCH_BUF (1 << 20) //每次批量写最大的buffer（1MB）,后台线程会把多个 AOF 命令合并到这个 buffer 里，减少写系统调用次数。
#define AOF_CQE_BATCH 128       //每次批量收割多少完成事件
#define AOF_IDLE_US 100         //后台线程没事的时候睡多久,AOF 后台线程空闲时 sleep 的时间，单位是微秒。
#define AOF_SQPOLL_IDLE_MS 1000 //内核 SQPOLL 线程没事时多久睡眠,SQPOLL 可以减少系统调用开销，但会多占用内核线程资源。

// ---------- io_uring  ----------
static struct io_uring g_uring; //iouring
static int g_aof_fd = -1;
static off_t g_aof_offset = 0;
static int g_uring_enabled = 0;

// ---------- always 模式?FILE* ----------
static FILE *aof_fp = NULL;

// ---------- 加载标志 ----------
static int loading_aof = 0;

typedef struct aof_write_req {
    char *buf;
    size_t cap;
    size_t len;
    size_t written;
    off_t file_off;
    int in_use;
} aof_write_req_t;


typedef struct aof_ring {
    char **slots;
    size_t *lens;
    size_t capacity;
    atomic_size_t write_idx;
    atomic_size_t read_idx;
    int stop;
    pthread_t thread;
    int sync_mode;
    atomic_int io_error;
} aof_ring_t;


static aof_ring_t g_aof_ring = {0};

// ---------- 存储引擎外部变量 ----------
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
#if ENABLE_VECTOR
extern kvs_vector_t  global_vector_tree;
#endif


#define AOF_FILE "./Persistence/kvstore.aof"
#define FULL_RDB_FILE "./Persistence/kvstore.rdb"

enum {
    TYPE_ARRAY = 1,
    TYPE_RBTREE = 2,
    TYPE_HASH = 3,
    TYPE_SKIPLIST = 4,
    TYPE_VECTOR = 5
};

static int persistence_mode = 2;  // 0:�? 1:RDB, 2:AOF

// ---------- RDB 相关函数----------
//红黑
static void rdb_write_kv_pair(FILE *fp, int type, const char *key, const char *value, long long expire_ms) 
{
    int keylen = strlen(key);
    int valuelen = strlen(value);
    fwrite(&type, sizeof(char), 1, fp);
    fwrite(&expire_ms, sizeof(long long), 1, fp);
    fwrite(&keylen, sizeof(int), 1, fp);
    fwrite(key, 1, keylen, fp);
    fwrite(&valuelen, sizeof(int), 1, fp);
    fwrite(value, 1, valuelen, fp);
}

static void rdb_save_rbtree_node(rbtree_node *node, FILE *fp, int type)
{
    if (node == global_rbtree.nil) return;
    rdb_save_rbtree_node(node->left, fp, type);
    rdb_write_kv_pair(fp, type, node->key, node->value, node->expire_ms);
    rdb_save_rbtree_node(node->right, fp, type);
}
//vector
#if ENABLE_VECTOR
static void rdb_write_vector_entry(FILE *fp, const char* question, const char *answer, int dim, const char *vector_str)
{
    char type = TYPE_VECTOR;
    int qlen = strlen(question);
    int alen = strlen(answer);
    int vlen = strlen(vector_str);

    fwrite(&type, sizeof(char), 1, fp);

    fwrite(&qlen, sizeof(int), 1, fp);
    fwrite(question, 1, qlen, fp);

    fwrite(&alen, sizeof(int), 1, fp);
    fwrite(answer, 1, alen, fp);

    fwrite(&dim, sizeof(int), 1, fp);

    fwrite(&vlen, sizeof(int), 1, fp);
    fwrite(vector_str, 1, vlen, fp);


}

static char *rdb_vector_to_string(float *vec, int dim) {
    if (!vec || dim <= 0) {
        return NULL;
    }

    size_t cap = (size_t)dim * 32 + 1;
    char *buf = (char *)kvs_malloc(cap);
    if (!buf) {
        return NULL;
    }

    buf[0] = '\0';

    size_t offset = 0;
    for (int i = 0; i < dim; i++) {
        int n = snprintf(
            buf + offset,
            cap - offset,
            "%s%.6f",
            i == 0 ? "" : ",",
            vec[i]
        );

        if (n < 0 || (size_t)n >= cap - offset) {
            kvs_free(buf);
            return NULL;
        }

        offset += (size_t)n;
    }

    return buf;
}
//中序遍历
static void rdb_save_vector_node(kvs_vector_t *inst, rbtree_node *node, FILE *fp) {
    if (!inst || !fp || node == inst->tree.nil) {
        return;
    }

    /*
     * 1. 先遍历左子树
     */
    rdb_save_vector_node(inst, node->left, fp);

    /*
     * 2. 处理当前节点
     * node->value 里存的是 vector_entry_t*
     */
    vector_entry_t *entry = (vector_entry_t *)node->value;
    if (entry &&
        entry->question &&
        entry->answer &&
        entry->vector &&
        entry->dim > 0) {

        char *vector_str = rdb_vector_to_string(entry->vector, entry->dim);
        if (vector_str) {
            rdb_write_vector_entry(
                fp,
                entry->question,
                entry->answer,
                entry->dim,
                vector_str
            );

            kvs_free(vector_str);
        }
    }

    /*
     * 3. 再遍历右子树
     */
    rdb_save_vector_node(inst, node->right, fp);
}



#endif  
static int rdb_save_to_fp(FILE *fp) {
    if (!fp) return -1;
#if ENABLE_ARRAY
    for (int i = 0; i < global_array.total; i++) {
        kvs_array_item_t *item = &global_array.table[i];
        if (item->key) {
            rdb_write_kv_pair(fp, TYPE_ARRAY, item->key, item->value, item->expire_ms);
        }
    }
#endif
#if ENABLE_RBTREE
    rdb_save_rbtree_node(global_rbtree.root, fp, TYPE_RBTREE);
#endif

#if ENABLE_VECTOR
    rdb_save_vector_node(&global_vector_tree, global_vector_tree.tree.root, fp);
#endif

#if ENABLE_HASH
    for (int i = 0; i < global_hash.max_slots; i++) {
        hashnode_t *node = global_hash.nodes[i];
        while (node) {
            rdb_write_kv_pair(fp, TYPE_HASH, node->key, node->value, node->expire_ms);
            node = node->next;
        }
    }
#endif
#if ENABLE_SKIPLIST
    skiplist_node_t *node = global_skiplist.header->forward[0];
    while (node) {
        rdb_write_kv_pair(fp, TYPE_SKIPLIST, node->key, node->value, node->expire_ms);
        node = node->forward[0];
    }
#endif
    return 0;
}

int rdb_save_to_file(const char *filename) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) return -1;
    int ret = rdb_save_to_fp(fp);
    fclose(fp);
    return ret;
}

int rdb_save_to_memory(char **out_buf, size_t *out_len) {
    char *buf = NULL;
    size_t len = 0;

    if (!out_buf || !out_len) return -1;

    FILE *fp = open_memstream(&buf, &len);
    if (!fp) return -1;

    int ret = rdb_save_to_fp(fp);
    if (fclose(fp) != 0) ret = -1;

    if (ret != 0) {
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = len;
    return 0;
}

int rdb_load_from_memory(const char *data, size_t len) {
    if (!data || len == 0) return 0;

    const char *ptr = data;
    const char *end = data + len;
    char *key_buf = NULL;
    char *val_buf = NULL;
    size_t key_cap = 0;
    size_t val_cap = 0;

    while (ptr < end) {
        if (ptr + 1 > end) break;
        char type = *ptr++;

#if ENABLE_VECTOR
        if (type == TYPE_VECTOR) {
            if (ptr + sizeof(int) > end) break;
            int qlen;
            memcpy(&qlen, ptr, sizeof(int));
            ptr += sizeof(int);
            if (qlen < 0 || ptr + qlen > end) break;
            const char *q = ptr;
            ptr += qlen;

            if (ptr + sizeof(int) > end) break;
            int alen;
            memcpy(&alen, ptr, sizeof(int));
            ptr += sizeof(int);
            if (alen < 0 || ptr + alen > end) break;
            const char *a = ptr;
            ptr += alen;

            if (ptr + sizeof(int) > end) break;
            int dim;
            memcpy(&dim, ptr, sizeof(int));
            ptr += sizeof(int);

            if (ptr + sizeof(int) > end) break;
            int vlen;
            memcpy(&vlen, ptr, sizeof(int));
            ptr += sizeof(int);
            if (vlen < 0 || ptr + vlen > end) break;
            const char *v = ptr;
            ptr += vlen;

            char *qbuf = (char *)kvs_malloc((size_t)qlen + 1);
            char *abuf = (char *)kvs_malloc((size_t)alen + 1);
            char *vbuf = (char *)kvs_malloc((size_t)vlen + 1);
            if (!qbuf || !abuf || !vbuf) {
                if (qbuf) kvs_free(qbuf);
                if (abuf) kvs_free(abuf);
                if (vbuf) kvs_free(vbuf);
                break;
            }

            memcpy(qbuf, q, (size_t)qlen);
            qbuf[qlen] = '\0';
            memcpy(abuf, a, (size_t)alen);
            abuf[alen] = '\0';
            memcpy(vbuf, v, (size_t)vlen);
            vbuf[vlen] = '\0';

            if (dim > 0) {
                kvs_vector_set(&global_vector_tree, qbuf, abuf, dim, vbuf);
            }

            kvs_free(qbuf);
            kvs_free(abuf);
            kvs_free(vbuf);
            continue;
        }
#endif

        if (ptr + sizeof(long long) > end) break;
        long long expire_ms;
        memcpy(&expire_ms, ptr, sizeof(long long));
        ptr += sizeof(long long);

        if (ptr + sizeof(int) > end) break;
        int keylen;
        memcpy(&keylen, ptr, sizeof(int));
        ptr += sizeof(int);
        if (keylen < 0 || ptr + keylen > end) break;
        const char *key = ptr;
        ptr += keylen;

        if (ptr + sizeof(int) > end) break;
        int valuelen;
        memcpy(&valuelen, ptr, sizeof(int));
        ptr += sizeof(int);
        if (valuelen < 0 || ptr + valuelen > end) break;
        const char *value = ptr;
        ptr += valuelen;

        if ((size_t)keylen + 1 > key_cap) {
            if (key_buf) kvs_free(key_buf);
            key_buf = (char *)kvs_malloc((size_t)keylen + 1);
            if (!key_buf) break;
            key_cap = (size_t)keylen + 1;
        }
        if ((size_t)valuelen + 1 > val_cap) {
            if (val_buf) kvs_free(val_buf);
            val_buf = (char *)kvs_malloc((size_t)valuelen + 1);
            if (!val_buf) break;
            val_cap = (size_t)valuelen + 1;
        }

        memcpy(key_buf, key, (size_t)keylen);
        key_buf[keylen] = '\0';
        memcpy(val_buf, value, (size_t)valuelen);
        val_buf[valuelen] = '\0';

        switch (type) {
#if ENABLE_ARRAY
            case TYPE_ARRAY:
                kvs_array_set(&global_array, key_buf, val_buf, expire_ms);
                break;
#endif
#if ENABLE_RBTREE
            case TYPE_RBTREE:
                kvs_rbtree_set(&global_rbtree, key_buf, val_buf, expire_ms);
                break;
#endif
#if ENABLE_HASH
            case TYPE_HASH:
                kvs_hash_set(&global_hash, key_buf, val_buf, expire_ms);
                break;
#endif
#if ENABLE_SKIPLIST
            case TYPE_SKIPLIST:
                kvs_skiplist_set(&global_skiplist, key_buf, val_buf, expire_ms);
                break;
#endif
            default:
                fprintf(stderr, "Unknown RDB type %d\n", type);
                break;
        }
    }

    if (key_buf) kvs_free(key_buf);
    if (val_buf) kvs_free(val_buf);
    return 0;
}


int rdb_load_files(void) {
    FILE *fp = fopen(FULL_RDB_FILE, "rb");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        return 0;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)file_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)file_size, fp);
    fclose(fp);
    if (nread != (size_t)file_size) {
        free(buf);
        return -1;
    }

    if (kvs_reset_data_engines() != 0) {
        free(buf);
        return -1;
    }

    int ret = rdb_load_from_memory(buf, (size_t)file_size);
    free(buf);
    return ret;
}
// ---------- AOF ----------
static int aof_reap_completions(struct io_uring *uring,
                                aof_write_req_t *reqs,
                                int *inflight,
                                int wait_one) {
    struct io_uring_cqe *cqes[AOF_CQE_BATCH];
    int total = 0;
    int resubmitted = 0;

    if (wait_one && *inflight > 0) {
        struct io_uring_cqe *cqe = NULL;
        int ret = io_uring_wait_cqe(uring, &cqe);
        if (ret < 0) {
            fprintf(stderr, "AOF io_uring_wait_cqe failed: %s\n", strerror(-ret));
            atomic_store(&g_aof_ring.io_error, 1);
            return ret;
        }
    }

    while (1) {
        int n = io_uring_peek_batch_cqe(uring, cqes, AOF_CQE_BATCH);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            struct io_uring_cqe *cqe = cqes[i];
            aof_write_req_t *req = (aof_write_req_t *)(uintptr_t)cqe->user_data;

            if (!req || !req->in_use) continue;

            if (cqe->res < 0) {
                fprintf(stderr, "AOF async write failed: %s\n", strerror(-cqe->res));
                atomic_store(&g_aof_ring.io_error, 1);
                req->in_use = 0;
                (*inflight)--;
                continue;
            }
            if (cqe->res == 0 && req->written < req->len) {
                fprintf(stderr, "AOF async write returned 0 before completion\n");
                atomic_store(&g_aof_ring.io_error, 1);
                req->in_use = 0;
                (*inflight)--;
                continue;
            }

            req->written += (size_t)cqe->res;
            if (req->written < req->len) {
                struct io_uring_sqe *sqe = io_uring_get_sqe(uring);
                if (!sqe) {
                    int ret = io_uring_submit(uring);
                    if (ret < 0) {
                        fprintf(stderr, "AOF submit before retry failed: %s\n", strerror(-ret));
                        atomic_store(&g_aof_ring.io_error, 1);
                        req->in_use = 0;
                        (*inflight)--;
                        continue;
                    }
                    sqe = io_uring_get_sqe(uring);
                }
                if (!sqe) {
                    fprintf(stderr, "AOF cannot acquire SQE for retry\n");
                    atomic_store(&g_aof_ring.io_error, 1);
                    req->in_use = 0;
                    (*inflight)--;
                    continue;
                }

                io_uring_prep_write(sqe,
                                    g_aof_fd,
                                    req->buf + req->written,
                                    req->len - req->written,
                                    req->file_off + (off_t)req->written);
                sqe->user_data = (uint64_t)(uintptr_t)req;
                resubmitted++;
            } else {
                req->in_use = 0;
                (*inflight)--;
            }
        }

        io_uring_cq_advance(uring, (unsigned)n);
        total += n;
    }

    if (resubmitted > 0) {
        int ret = io_uring_submit(uring);
        if (ret < 0) {
            fprintf(stderr, "AOF retry submit failed: %s\n", strerror(-ret));
            atomic_store(&g_aof_ring.io_error, 1);
            for (int i = 0; i < AOF_IO_DEPTH; i++) reqs[i].in_use = 0;
            *inflight = 0;
            return ret;
        }
    }

    return total;
}

static aof_write_req_t *aof_find_free_req(aof_write_req_t *reqs) {
    for (int i = 0; i < AOF_IO_DEPTH; i++) {
        if (!reqs[i].in_use) return &reqs[i];
    }
    return NULL;
}

static int aof_fill_batch(aof_ring_t *ring,
                          aof_write_req_t *req,
                          size_t *local_read_idx) {
    size_t write_idx = atomic_load_explicit(&ring->write_idx, memory_order_acquire);
    size_t batch_len = 0;

    while (*local_read_idx < write_idx) {
        size_t idx = *local_read_idx % ring->capacity;
        char *data = ring->slots[idx];
        size_t len = ring->lens[idx];

        if (!data) {
            (*local_read_idx)++;
            continue;
        }

        if (len > req->cap && batch_len == 0) {
            char *new_buf = (char *)realloc(req->buf, len);
            if (!new_buf) {
                fprintf(stderr, "AOF large command buffer allocation failed: %zu\n", len);
                atomic_store(&ring->io_error, 1);
                return -1;
            }
            req->buf = new_buf;
            req->cap = len;
        }

        if (batch_len > 0 && batch_len + len > req->cap) break;

        memcpy(req->buf + batch_len, data, len);
        batch_len += len;

        kvs_free(data);
        ring->slots[idx] = NULL;
        ring->lens[idx] = 0;
        (*local_read_idx)++;
    }

    req->len = batch_len;
    req->written = 0;
    return batch_len > 0 ? 1 : 0;
}

static void *aof_worker(void *arg) {
    aof_ring_t *ring = &g_aof_ring;

    if (ring->sync_mode == 2) {  //fwrite
        aof_fp = fopen(AOF_FILE, "a");
        if (!aof_fp) {
            fprintf(stderr, "AOF always worker: failed to open file\n");
            atomic_store(&ring->io_error, 1);
            return NULL;
        }
        setbuf(aof_fp, NULL);

        while (!ring->stop || atomic_load_explicit(&ring->read_idx, memory_order_acquire) < atomic_load_explicit(&ring->write_idx, memory_order_acquire)) {
            size_t read_idx = atomic_load_explicit(&ring->read_idx, memory_order_acquire);
            size_t write_idx = atomic_load_explicit(&ring->write_idx, memory_order_acquire);

            if (write_idx == read_idx) {
                usleep(AOF_IDLE_US);
                continue;
            }

            size_t idx = read_idx % ring->capacity;
            char *data = ring->slots[idx];
            size_t len = ring->lens[idx];

            if (data) {
                if (fwrite(data, 1, len, aof_fp) != len || fflush(aof_fp) != 0 || fsync(fileno(aof_fp)) != 0) {
                    fprintf(stderr, "AOF always write/fsync failed: %s\n", strerror(errno));
                    atomic_store(&ring->io_error, 1);
                }
                kvs_free(data);
                ring->slots[idx] = NULL;
                ring->lens[idx] = 0;
            }

            atomic_store_explicit(&ring->read_idx, read_idx + 1, memory_order_release);
        }

        if (aof_fp) fclose(aof_fp);
        aof_fp = NULL;
        return NULL;
    }

    aof_write_req_t reqs[AOF_IO_DEPTH];//同时飞行的请求数
    memset(reqs, 0, sizeof(reqs));

    for (int i = 0; i < AOF_IO_DEPTH; i++) {//分配空间
        reqs[i].buf = (char *)malloc(AOF_BATCH_BUF);
        if (!reqs[i].buf) {
            fprintf(stderr, "AOF batch buffer allocation failed\n");
            atomic_store(&ring->io_error, 1);
            for (int j = 0; j < i; j++) free(reqs[j].buf);
            return NULL;
        }
        reqs[i].cap = AOF_BATCH_BUF;
    }

    int inflight = 0;
    size_t local_read_idx = atomic_load_explicit(&ring->read_idx, memory_order_acquire);

    while (!ring->stop || local_read_idx < atomic_load_explicit(&ring->write_idx, memory_order_acquire) || inflight > 0) { //只要服务没要求停止，或者队列里还有 AOF 数据没取完，或者已经提交的 io_uring 写请求还没完成，后台线程就继续循环。
        aof_reap_completions(&g_uring, reqs, &inflight, 0);//用于回收CQ，哦，这里面还会判断有没有写完

        int submitted_now = 0;
        while (local_read_idx < atomic_load_explicit(&ring->write_idx, memory_order_acquire) && inflight < AOF_IO_DEPTH) {
            aof_write_req_t *req = aof_find_free_req(reqs);
            if (!req) break;

            int fill = aof_fill_batch(ring, req, &local_read_idx);
            if (fill < 0) break;
            if (fill == 0) break;

            struct io_uring_sqe *sqe = io_uring_get_sqe(&g_uring);
            if (!sqe) {
                int ret = io_uring_submit(&g_uring);
                if (ret < 0) {
                    fprintf(stderr, "AOF io_uring_submit on SQ full failed: %s\n", strerror(-ret));
                    atomic_store(&ring->io_error, 1);
                    break;
                }
                sqe = io_uring_get_sqe(&g_uring);
            }
            if (!sqe) break;

            req->file_off = g_aof_offset;
            g_aof_offset += (off_t)req->len;
            req->in_use = 1;

            io_uring_prep_write(sqe, g_aof_fd, req->buf, req->len, req->file_off);
            sqe->user_data = (uint64_t)(uintptr_t)req;

            inflight++;
            submitted_now++;
        }

        if (submitted_now > 0) {
            int ret = io_uring_submit(&g_uring);
            if (ret < 0) {
                fprintf(stderr, "AOF io_uring_submit failed: %s\n", strerror(-ret));
                atomic_store(&ring->io_error, 1);
                for (int i = 0; i < AOF_IO_DEPTH; i++) reqs[i].in_use = 0;
                inflight = 0;
                ring->stop = 1;
            }
            atomic_store_explicit(&ring->read_idx, local_read_idx, memory_order_release);
            continue;
        }

        if (inflight > 0) {
            aof_reap_completions(&g_uring, reqs, &inflight, 1);
        } else {
            usleep(AOF_IDLE_US);
        }
    }

    for (int i = 0; i < AOF_IO_DEPTH; i++) free(reqs[i].buf);
    return NULL;
}

// ---------- AOF ----------
static int aof_load_file(void) {
    FILE *fp = fopen(AOF_FILE, "rb");
    if (!fp) return 0;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long file_size = ftell(fp);
    if (file_size <= 0) {
        fclose(fp);
        return 0;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)file_size);
    if (!buf) {
        fclose(fp);
        return -1;
    }

    size_t nread = fread(buf, 1, (size_t)file_size, fp);
    fclose(fp);
    if (nread != (size_t)file_size) {
        free(buf);
        return -1;
    }

    loading_aof = 1;
    char *ptr = buf;
    char *end = buf + file_size;
    char *resp = NULL;

    while (ptr < end) {
        size_t cmd_len;
        int ret = resp_parse_one_command(ptr, end - ptr, &cmd_len);
        if (ret <= 0) break;
        kvs_protocol(-1, ptr, cmd_len, &resp);
        if (resp) {
            kvs_free(resp);
            resp = NULL;
        }
        ptr += cmd_len;
    }

    loading_aof = 0;
    free(buf);
    return 0;
}

// ---------- API ----------
int kvs_persistence_init(void) {
    mkdir("./Persistence", 0755);
    if (!g_config.appendonly) return 0;

    int mode = g_config.appendfsync;
    aof_ring_t *ring = &g_aof_ring;

    ring->capacity = AOF_RING_CAPACITY;
    ring->slots = (char **)kvs_malloc(sizeof(char *) * ring->capacity);
    ring->lens = (size_t *)kvs_malloc(sizeof(size_t) * ring->capacity);
    if (!ring->slots || !ring->lens) {
        fprintf(stderr, "Failed to allocate AOF ring buffer\n");
        if (ring->slots) kvs_free(ring->slots);
        if (ring->lens) kvs_free(ring->lens);
        ring->slots = NULL;
        ring->lens = NULL;
        return -1;
    }

    memset(ring->slots, 0, sizeof(char *) * ring->capacity);
    memset(ring->lens, 0, sizeof(size_t) * ring->capacity);
    atomic_init(&ring->write_idx, 0);
    atomic_init(&ring->read_idx, 0);
    atomic_init(&ring->io_error, 0);
    ring->stop = 0;
    ring->sync_mode = mode;

    if (mode == 2) {//fwrite
        if (pthread_create(&ring->thread, NULL, aof_worker, NULL) != 0) {
            perror("pthread_create always");
            return -1;
        }
        return 0;
    }

    struct io_uring_params params = {0};
    params.flags = IORING_SETUP_SQPOLL;
    params.sq_thread_idle = AOF_SQPOLL_IDLE_MS;

    int ret = io_uring_queue_init_params(AOF_URING_ENTRIES, &g_uring, &params);
    if (ret < 0) {
        fprintf(stderr, "AOF SQPOLL init failed: %s, fallback to normal io_uring\n", strerror(-ret));
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(AOF_URING_ENTRIES, &g_uring, &params);
        if (ret < 0) {
            fprintf(stderr, "AOF io_uring init failed: %s\n", strerror(-ret));
            return -1;
        }
    }

    g_uring_enabled = 1;

    printf("AOF io_uring initialized: entries=%d, depth=%d, batch=%d, sqpoll=%s\n",
           AOF_URING_ENTRIES,
           AOF_IO_DEPTH,
           AOF_BATCH_BUF,
           (params.flags & IORING_SETUP_SQPOLL) ? "on" : "off");

    g_aof_fd = open(AOF_FILE, O_WRONLY | O_CREAT, 0644);
    if (g_aof_fd < 0) {
        io_uring_queue_exit(&g_uring);
        g_uring_enabled = 0;
        return -1;
    }

    g_aof_offset = lseek(g_aof_fd, 0, SEEK_END);
    if (g_aof_offset < 0) {
        close(g_aof_fd);
        g_aof_fd = -1;
        io_uring_queue_exit(&g_uring);
        g_uring_enabled = 0;
        return -1;
    }

    if (pthread_create(&ring->thread, NULL, aof_worker, NULL) != 0) {
        perror("pthread_create no");
        close(g_aof_fd);
        g_aof_fd = -1;
        io_uring_queue_exit(&g_uring);
        g_uring_enabled = 0;
        return -1;
    }

    return 0;
}

void kvs_persistence_close(void) {
    aof_ring_t *ring = &g_aof_ring;
    if (!g_config.appendonly) return;

    ring->stop = 1;
    pthread_join(ring->thread, NULL);

    for (size_t i = 0; i < ring->capacity; i++) {
        if (ring->slots[i]) {
            kvs_free(ring->slots[i]);
            ring->slots[i] = NULL;
        }
    }
    kvs_free(ring->slots);
    kvs_free(ring->lens);
    ring->slots = NULL;
    ring->lens = NULL;

    if (aof_fp) {
        fclose(aof_fp);
        aof_fp = NULL;
    }
    if (g_aof_fd >= 0) {
        fsync(g_aof_fd);
        close(g_aof_fd);
        g_aof_fd = -1;
    }
    if (g_uring_enabled) {
        io_uring_queue_exit(&g_uring);
        g_uring_enabled = 0;
    }
}

int kvs_persistence_append(const char *cmd, int len) {
    if (loading_aof || !g_config.appendonly) return 0;
    if (!cmd || len <= 0) return -1;

    aof_ring_t *ring = &g_aof_ring;
    if (atomic_load(&ring->io_error)) return -1;

    char *data = (char *)kvs_malloc((size_t)len);
    if (!data) return -1;
    memcpy(data, cmd, (size_t)len);

    size_t write_idx = atomic_load_explicit(&ring->write_idx, memory_order_relaxed);
    size_t read_idx = atomic_load_explicit(&ring->read_idx, memory_order_acquire);

    while (write_idx - read_idx >= ring->capacity) {
        usleep(AOF_IDLE_US);
        if (atomic_load(&ring->io_error)) {
            kvs_free(data);
            return -1;
        }
        read_idx = atomic_load_explicit(&ring->read_idx, memory_order_acquire);
    }

    size_t idx = write_idx % ring->capacity;
    ring->slots[idx] = data;
    ring->lens[idx] = (size_t)len;
    atomic_store_explicit(&ring->write_idx, write_idx + 1, memory_order_release);

    return 0;
}
int kvs_persistence_bgsave(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    } else if (pid == 0) {
        int ret = rdb_save_to_file(FULL_RDB_FILE);
        _exit(ret == 0 ? 0 : 1);
    } else {
        return 0;
    }
}

int kvs_persistence_load(void) {
    if (persistence_mode == 2) {
        aof_load_file();
    }
    else if (persistence_mode == 1)
    {
        rdb_load_files();
    }
    return 0;
}
