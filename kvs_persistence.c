// #define _GNU_SOURCE
// #include <fcntl.h>
#include "kvstore.h"
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <liburing.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>

#include <stdatomic.h>

#define MAX_BATCH 2048   




// ---------- io_uring 相关（仅 no 模式使用）----------
static struct io_uring g_uring;
static int g_aof_fd = -1;
static off_t g_aof_offset = 0;

// ---------- always 模式的 FILE* ----------
static FILE *aof_fp = NULL;

// ---------- 加载标志 ----------
static int loading_aof = 0;




// 无锁环形缓冲区结构（SPSC）
typedef struct aof_ring {
    char **slots;                   // 指向命令数据的指针数组
    size_t *lens;                   // 每条命令的长度
    size_t capacity;
    atomic_size_t write_idx;        // 生产者写位置
    atomic_size_t read_idx;         // 消费者读位置

    int stop;
    pthread_t thread;
    int sync_mode;                  // 0: no (io_uring), 2: always (同步写)

    // 添加批量控制字段
    int batch_threshold;
    int timeout_us;
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

static int persistence_mode = 2;  // 0:无, 1:RDB, 2:AOF

// ---------- RDB 相关函数----------
//红黑树
static void rdb_write_kv_pair(FILE *fp, int type, const char *key, const char *value, long long expire_ms) {
    int keylen = strlen(key);
    int valuelen = strlen(value);
    fwrite(&type, sizeof(char), 1, fp);
    fwrite(&expire_ms, sizeof(long long), 1, fp);
    fwrite(&keylen, sizeof(int), 1, fp);
    fwrite(key, 1, keylen, fp);
    fwrite(&valuelen, sizeof(int), 1, fp);
    fwrite(value, 1, valuelen, fp);
}

static void rdb_save_rbtree_node(rbtree_node *node, FILE *fp, int type) {
    if (node == global_rbtree.nil) return;
    rdb_save_rbtree_node(node->left, fp, type);
    rdb_write_kv_pair(fp, type, node->key, node->value, node->expire_ms);
    rdb_save_rbtree_node(node->right, fp, type);
}
//vector ，和红黑树不同
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

    /*
     * 每个 float 预留 32 字节基本够用：
     * 例如 -123.456789,
     */
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

int rdb_load_files(void) {
    int fd = open(FULL_RDB_FILE, O_RDONLY);
    if (fd < 0) return 0;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return 0;
    }

    char *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;

    char *ptr = map, *end = map + st.st_size;
    char *key_buf = NULL, *val_buf = NULL;
    size_t key_cap = 0, val_cap = 0;

    while (ptr < end) {

        //先判断是不是Vector
        if(ptr + 1 > end) break;
        char type = *ptr++;
    #if ENABLE_VECTOR
        if(type == TYPE_VECTOR)
        {
            if (ptr + sizeof(int) > end) break;
            int qlen;
            memcpy(&qlen, ptr, sizeof(int));
            ptr += sizeof(int);

            if (qlen < 0 || ptr + qlen > end) break;
            char *q = ptr;
            ptr += qlen;

            if (ptr + sizeof(int) > end) break;
            int alen;
            memcpy(&alen, ptr, sizeof(int));
            ptr += sizeof(int);

            if (alen < 0 || ptr + alen > end) break;
            char *a = ptr;
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
            char *v = ptr;
            ptr += vlen;

            char *qbuf = (char *)kvs_malloc(qlen + 1);
            char *abuf = (char *)kvs_malloc(alen + 1);
            char *vbuf = (char *)kvs_malloc(vlen + 1);

            if (!qbuf || !abuf || !vbuf) {
                if (qbuf) kvs_free(qbuf);
                if (abuf) kvs_free(abuf);
                if (vbuf) kvs_free(vbuf);
                break;
            }

            memcpy(qbuf, q, qlen);
            qbuf[qlen] = '\0';
            memcpy(abuf, a, alen);
            abuf[alen] = '\0';
            memcpy(vbuf, v, vlen);
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

        if (ptr + 1 + sizeof(long long) > end) break;
        // char type = *ptr++;
        long long expire_ms;
        memcpy(&expire_ms, ptr, sizeof(long long));
        ptr += sizeof(long long);

        if (ptr + sizeof(int) > end) break;
        int keylen;
        memcpy(&keylen, ptr, sizeof(int));
        ptr += sizeof(int);
        if (keylen < 0 || ptr + keylen > end) break;
        char *key = ptr;
        ptr += keylen;

        if (ptr + sizeof(int) > end) break;
        int valuelen;
        memcpy(&valuelen, ptr, sizeof(int));
        ptr += sizeof(int);
        if (valuelen < 0 || ptr + valuelen > end) break;
        char *value = ptr;
        ptr += valuelen;

        if (keylen + 1 > key_cap) {
            if (key_buf) kvs_free(key_buf);
            key_buf = kvs_malloc(keylen + 1);
            if (!key_buf) break;
            key_cap = keylen + 1;
        }
        if (valuelen + 1 > val_cap) {
            if (val_buf) kvs_free(val_buf);
            val_buf = kvs_malloc(valuelen + 1);
            if (!val_buf) break;
            val_cap = valuelen + 1;
        }
        memcpy(key_buf, key, keylen);
        key_buf[keylen] = '\0';
        memcpy(val_buf, value, valuelen);
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
    munmap(map, st.st_size);
    return 0;
}




// ---------- AOF 后台工作线程（统一处理 always 和 no 模式）----------
static void *aof_worker(void *arg) {
    aof_ring_t *ring = &g_aof_ring;

    // ---------------- always 模式（不动） ----------------
    if (ring->sync_mode == 2) {
        aof_fp = fopen(AOF_FILE, "a");
        if (!aof_fp) {
            fprintf(stderr, "AOF always worker: failed to open file\n");
            return NULL;
        }
        setbuf(aof_fp, NULL);

        while (!ring->stop) {
            size_t write_idx = atomic_load(&ring->write_idx);
            size_t read_idx  = atomic_load(&ring->read_idx);

            if (write_idx == read_idx) {
                usleep(100);
                continue;
            }

            size_t idx = read_idx % ring->capacity;
            char *data = ring->slots[idx];
            size_t len = ring->lens[idx];

            if (data) {
                fwrite(data, 1, len, aof_fp);
                fflush(aof_fp);
                fsync(fileno(aof_fp));

                kvs_free(data);
                ring->slots[idx] = NULL;
            }

            atomic_store(&ring->read_idx, read_idx + 1);
        }

        if (aof_fp) fclose(aof_fp);
        return NULL;
    }

    // ---------------- no 模式（重写核心逻辑） ----------------
    struct io_uring *ring_uring = &g_uring;

    #define AOF_BATCH_BUF (1 << 20)   // 1MB

    char *batch_buf;
    if (posix_memalign((void **)&batch_buf, 4096, AOF_BATCH_BUF) != 0) {
        fprintf(stderr, "batch_buf alloc failed\n");
        return NULL;
    }

    size_t batch_len = 0;
    size_t local_read_idx = 0;

    while (!ring->stop) {
        size_t write_idx = atomic_load(&ring->write_idx);

        if (local_read_idx == 0) {
            local_read_idx = atomic_load(&ring->read_idx);
        }

        if (write_idx == local_read_idx) {
            usleep(100);
            continue;
        }

        // ----------- 拼接 batch -----------
        batch_len = 0;

        while (local_read_idx < write_idx) {
            size_t idx = local_read_idx % ring->capacity;
            char *data = ring->slots[idx];
            size_t len = ring->lens[idx];

            if (!data) {
                local_read_idx++;
                continue;
            }

            // 放不下就先提交
            if (batch_len + len > AOF_BATCH_BUF) {
                break;
            }

            memcpy(batch_buf + batch_len, data, len);
            batch_len += len;

            kvs_free(data);
            ring->slots[idx] = NULL;

            local_read_idx++;
        }

        if (batch_len == 0) continue;

        // ----------- 对齐（为 O_DIRECT 准备）-----------
        // size_t aligned_len = (batch_len + 4095) & ~4095;
        // if (aligned_len > batch_len) {
        //     memset(batch_buf + batch_len, 0, aligned_len - batch_len);
        // }

        // ----------- 提交一次 write -----------
        struct io_uring_sqe *sqe = io_uring_get_sqe(ring_uring);

        if (sqe) {
            // off_t offset = __sync_fetch_and_add(&g_aof_offset, aligned_len);

            off_t offset = __sync_fetch_and_add(&g_aof_offset, batch_len);
            io_uring_prep_write(sqe, g_aof_fd, batch_buf, batch_len, offset);   
            io_uring_submit_and_wait(ring_uring, 1);

            // 回收 CQE
            struct io_uring_cqe *cqe;
            if (io_uring_peek_cqe(ring_uring, &cqe) == 0) {
                if (cqe->res < 0) {
                    fprintf(stderr, "AOF write error: %d\n", cqe->res);
                }
                io_uring_cqe_seen(ring_uring, cqe);
            }
        } else {
            // fallback（极少发生）
            // off_t offset = __sync_fetch_and_add(&g_aof_offset, aligned_len);
            // ssize_t n = pwrite(g_aof_fd, batch_buf, aligned_len, offset);
            // (void)n;
        }

        // 更新读指针
        atomic_store(&ring->read_idx, local_read_idx);
    }

    free(batch_buf);
    return NULL;
}
// ---------- AOF 加载（mmap）----------
static int aof_load_file(void) {
    FILE *fp = fopen(AOF_FILE, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    int fd = fileno(fp);
    char *map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        fclose(fp);
        return -1;
    }

    loading_aof = 1;
    char *ptr = map;
    char *end = map + file_size;
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
    munmap(map, file_size);
    fclose(fp);
    return 0;
}

// ---------- 公共 API ----------


int kvs_persistence_init(void) {
    mkdir("./Persistence", 0755);
    if (!g_config.appendonly) return 0;//是否开启aof

    // persistence_mode = 2;
    int mode = g_config.appendfsync;  // 0=no, 2=always

    // 初始化环形缓冲区
    aof_ring_t *ring = &g_aof_ring;
    ring->capacity = AOF_RING_CAPACITY;//65536
    ring->slots = (char **)kvs_malloc(sizeof(char *) * ring->capacity);
    ring->lens = (size_t *)kvs_malloc(sizeof(size_t) * ring->capacity);
    if (!ring->slots || !ring->lens) {
        fprintf(stderr, "Failed to allocate AOF ring buffer\n");
        return -1;
    }
    memset(ring->slots, 0, sizeof(char *) * ring->capacity);
    memset(ring->lens, 0, sizeof(size_t) * ring->capacity);
    atomic_init(&ring->write_idx, 0);//声明为原子变量
    atomic_init(&ring->read_idx, 0);

    ring->stop = 0;//stop=1代表需要停止了
    ring->sync_mode = mode;

    if (mode == 2) {
        // always 模式：创建后台线程，不使用 io_uring
        if (pthread_create(&ring->thread, NULL, aof_worker, NULL) != 0) {
            perror("pthread_create always");
            return -1;
        }
    } else {
        // no 模式：初始化 io_uring + 后台线程
        struct io_uring_params params = {0};
        params.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
        params.sq_thread_idle = 1000;  // 空闲 1ms 后睡眠，可调低如 100
        int ret = io_uring_queue_init_params(MAX_BATCH * 4, &g_uring, &params);

        if (ret < 0) { perror("io_uring_queue_init_params"); return -1; }

        g_aof_fd = open(AOF_FILE, O_WRONLY | O_CREAT  , 0644);
        if (g_aof_fd < 0) {
            io_uring_queue_exit(&g_uring);
            return -1;
        }

        
        g_aof_offset = lseek(g_aof_fd, 0, SEEK_END);

        if (pthread_create(&ring->thread, NULL, aof_worker, NULL) != 0) {
            perror("pthread_create no");
            return -1;
        }
    }
    return 0;
}

void kvs_persistence_close(void) {
    aof_ring_t *ring = &g_aof_ring;
    if (!g_config.appendonly) return;

    // 通知工作线程停止
    ring->stop = 1;
    // 唤醒可能阻塞在 sem_wait 的线程
    pthread_join(ring->thread, NULL);

    // 清理环形缓冲区
    for (size_t i = 0; i < ring->capacity; i++) {
        if (ring->slots[i]) {
            kvs_free(ring->slots[i]);
        }
    }
    kvs_free(ring->slots);
    kvs_free(ring->lens);


    if (aof_fp) fclose(aof_fp);
    if (g_aof_fd >= 0) close(g_aof_fd);
    io_uring_queue_exit(&g_uring);
}



int kvs_persistence_append(const char *cmd, int len) {
    if (loading_aof || !g_config.appendonly) return 0;

    aof_ring_t *ring = &g_aof_ring;

    size_t write_idx = atomic_load(&ring->write_idx);
    size_t read_idx = atomic_load(&ring->read_idx);
    // 环形缓冲区满则降级同步写（不阻塞业务）
    if (write_idx - read_idx >= ring->capacity) {
        printf("降级\n");
        if (ring->sync_mode == 0) {
            ssize_t n = pwrite(g_aof_fd, cmd, len,
                               __sync_fetch_and_add(&g_aof_offset, len));
            (void)n;
        } else {
            fwrite(cmd, 1, len, aof_fp);
            fflush(aof_fp);
            fsync(fileno(aof_fp));
        }
        return 0;
    }

    // 分配内存并拷贝命令
    char *data = (char *)kvs_malloc(len);
    if (!data) return -1;
    memcpy(data, cmd, len);

    size_t idx = write_idx % ring->capacity;
    ring->slots[idx] = data;
    ring->lens[idx] = len;

    atomic_store(&ring->write_idx, write_idx + 1);
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