// kvs_command.c
#include "kvstore.h"
#include <string.h>




#include <sys/wait.h>

// static int set_count = 0;
// static int set_threshold = 1000000;   // 每 10000 次 SET 触发一次 bgsave
// static int bgsave_running = 0;

// static void check_and_trigger_bgsave(void) {
//     set_count++;
//     if (set_count >= set_threshold) {
//         // printf("bgsvae\n");
//         kvs_persistence_bgsave();
//         set_count = 0;   // 触发后重置计数器
//     }
// }


// 全局存储引擎实例（外部声明，实际定义在 kvstore.c 或其他文件中）
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
extern kvs_vector_t global_vector_tree;
#endif

// 配置文件全局变量
extern kvs_config_t g_config;

//---------------------------------------------------
int kvs_get_client_addr(int fd, struct sockaddr_in *addr) {
    socklen_t len = sizeof(struct sockaddr_in);
    return getpeername(fd, (struct sockaddr*)addr, &len);
}


// 复制标志（从节点接收命令时避免循环）
extern __thread int replicating;

// 命令名称表（内部使用）
static const char *command[] = {
    "SET", "GET", "DEL", "MOD", "EXIST",
    "RSET", "RGET", "RDEL", "RMOD", "REXIST",
    "HSET", "HGET", "HDEL", "HMOD", "HEXIST",
    "SKSET", "SKGET", "SKDEL", "SKMOD", "SKEXIST",
    "BGSAVE",
    "SYNC", "FINSYNC",
    "EXPIRE", "TTL", "SETEX",
    "REXPIRE", "RTTL", "RSETEX",
    "HEXPIRE", "HTTL", "HSETEX",
    "SKEXPIRE", "SKTTL", "SKSETEX",
    "VSET", "VGET", "VSEARCH",
    "PING"
};

// 命令枚举（内部使用）
enum {
    KVS_CMD_START = 0,
    KVS_CMD_SET = KVS_CMD_START,
    KVS_CMD_GET,
    KVS_CMD_DEL,
    KVS_CMD_MOD,
    KVS_CMD_EXIST,
    KVS_CMD_RSET,
    KVS_CMD_RGET,
    KVS_CMD_RDEL,
    KVS_CMD_RMOD,
    KVS_CMD_REXIST,
    KVS_CMD_HSET,
    KVS_CMD_HGET,
    KVS_CMD_HDEL,
    KVS_CMD_HMOD,
    KVS_CMD_HEXIST,
    KVS_CMD_SKSET,
    KVS_CMD_SKGET,
    KVS_CMD_SKDEL,
    KVS_CMD_SKMOD,
    KVS_CMD_SKEXIST,
    KVS_CMD_BGSAVE,
    KVS_CMD_SYNC,
    KVS_CMD_FINSYNC,
    KVS_CMD_EXPIRE,
    KVS_CMD_TTL,
    KVS_CMD_SETEX,
    KVS_CMD_REXPIRE,
    KVS_CMD_RTTL,
    KVS_CMD_RSETEX,
    KVS_CMD_HEXPIRE,
    KVS_CMD_HTTL,
    KVS_CMD_HSETEX,
    KVS_CMD_SKEXPIRE,
    KVS_CMD_SKTTL,
    KVS_CMD_SKSETEX,
    KVS_CMD_VSET,
    KVS_CMD_VGET,
    KVS_CMD_VSEARCH,
    KVS_CMD_PING,
    KVS_CMD_COUNT
};

typedef struct {
    int min_argc;
    int max_argc;
    int is_write;
    int persist_on_success;
} kvs_cmd_meta_t;

static const kvs_cmd_meta_t command_meta[KVS_CMD_COUNT] = {
    [KVS_CMD_SET]      = {3, 3, 1, 1},
    [KVS_CMD_GET]      = {2, 2, 0, 0},
    [KVS_CMD_DEL]      = {2, 2, 1, 1},
    [KVS_CMD_MOD]      = {3, 3, 1, 1},
    [KVS_CMD_EXIST]    = {2, 2, 0, 0},
    [KVS_CMD_RSET]     = {3, 3, 1, 1},
    [KVS_CMD_RGET]     = {2, 2, 0, 0},
    [KVS_CMD_RDEL]     = {2, 2, 1, 1},
    [KVS_CMD_RMOD]     = {3, 3, 1, 1},
    [KVS_CMD_REXIST]   = {2, 2, 0, 0},
    [KVS_CMD_HSET]     = {3, 3, 1, 1},
    [KVS_CMD_HGET]     = {2, 2, 0, 0},
    [KVS_CMD_HDEL]     = {2, 2, 1, 1},
    [KVS_CMD_HMOD]     = {3, 3, 1, 1},
    [KVS_CMD_HEXIST]   = {2, 2, 0, 0},
    [KVS_CMD_SKSET]    = {3, 3, 1, 1},
    [KVS_CMD_SKGET]    = {2, 2, 0, 0},
    [KVS_CMD_SKDEL]    = {2, 2, 1, 1},
    [KVS_CMD_SKMOD]    = {3, 3, 1, 1},
    [KVS_CMD_SKEXIST]  = {2, 2, 0, 0},
    [KVS_CMD_BGSAVE]   = {1, 1, 0, 0},
    [KVS_CMD_SYNC]     = {1, 1, 0, 0},
    [KVS_CMD_FINSYNC]  = {1, 1, 0, 0},
    [KVS_CMD_EXPIRE]   = {3, 3, 1, 1},
    [KVS_CMD_TTL]      = {2, 2, 0, 0},
    [KVS_CMD_SETEX]    = {4, 4, 1, 1},
    [KVS_CMD_REXPIRE]  = {3, 3, 1, 1},
    [KVS_CMD_RTTL]     = {2, 2, 0, 0},
    [KVS_CMD_RSETEX]   = {4, 4, 1, 1},
    [KVS_CMD_HEXPIRE]  = {3, 3, 1, 1},
    [KVS_CMD_HTTL]     = {2, 2, 0, 0},
    [KVS_CMD_HSETEX]   = {4, 4, 1, 1},
    [KVS_CMD_SKEXPIRE] = {3, 3, 1, 1},
    [KVS_CMD_SKTTL]    = {2, 2, 0, 0},
    [KVS_CMD_SKSETEX]  = {4, 4, 1, 1},
    [KVS_CMD_VSET]     = {5, 5, 1, 1},
    [KVS_CMD_VGET]     = {2, 4, 0, 0},
    [KVS_CMD_VSEARCH]  = {5, 5, 0, 0},
    [KVS_CMD_PING]     = {1, 1, 0, 0},
};

static int kvs_reply_error(char **out_buf, const char *msg)
{
    int len = (int)strlen(msg);
    *out_buf = kvs_malloc(len + 1);
    if (!*out_buf) {
        return -2;
    }
    memcpy(*out_buf, msg, (size_t)len + 1);
    return len;
}

static int is_write_command(int cmd)
{
    return cmd >= 0 && cmd < KVS_CMD_COUNT && command_meta[cmd].is_write;
}

static int command_argc_ok(int cmd, int argc)
{
    if (cmd < 0 || cmd >= KVS_CMD_COUNT) {
        return 0;
    }
    return argc >= command_meta[cmd].min_argc &&
           argc <= command_meta[cmd].max_argc;
}

static int command_result_changed_data(int cmd, int ret)
{
    if (ret < 0) {
        return 0;
    }

    switch (cmd) {
    case KVS_CMD_SET:
    case KVS_CMD_RSET:
    case KVS_CMD_HSET:
    case KVS_CMD_SKSET:
    case KVS_CMD_SETEX:
    case KVS_CMD_RSETEX:
    case KVS_CMD_HSETEX:
    case KVS_CMD_SKSETEX:
    case KVS_CMD_VSET:
        return 1;

    case KVS_CMD_DEL:
    case KVS_CMD_RDEL:
    case KVS_CMD_HDEL:
    case KVS_CMD_SKDEL:
    case KVS_CMD_MOD:
    case KVS_CMD_RMOD:
    case KVS_CMD_HMOD:
    case KVS_CMD_SKMOD:
    case KVS_CMD_EXPIRE:
    case KVS_CMD_REXPIRE:
    case KVS_CMD_HEXPIRE:
    case KVS_CMD_SKEXPIRE:
        return ret == 0;

    default:
        return 0;
    }
}

static void persist_command_if_changed(int cmd, int ret, const char *raw_msg, int raw_len)
{
    if (cmd < 0 || cmd >= KVS_CMD_COUNT ||
        !command_meta[cmd].persist_on_success ||
        !command_result_changed_data(cmd, ret) ||
        !raw_msg || raw_len <= 0) {
        return;
    }

    kvs_persistence_append(raw_msg, raw_len);
}

int kvs_execute_command(int fd, int argc, char **argv, const char *raw_msg, int raw_len, char **out_buf) {

    *out_buf = NULL;

    if (argc < 1) {
        return kvs_reply_error(out_buf, "-ERR wrong number of arguments\r\n");
    }

    int cmd = -1;
    for (int i = 0; i < KVS_CMD_COUNT; i++) {
        if (strcmp(argv[0], command[i]) == 0) {
            cmd = i;
            break;
        }
    }

    if (cmd == -1) {
        int len = snprintf(NULL, 0, "-ERR unknown command '%s'\r\n", argv[0]);
        *out_buf = kvs_malloc(len + 1);
        if (!*out_buf) return -2;
        return sprintf(*out_buf, "-ERR unknown command '%s'\r\n", argv[0]);
    }

    if (!command_argc_ok(cmd, argc)) {
        return kvs_reply_error(out_buf, "-ERR wrong number of arguments\r\n");
    }

    if (g_config.slave_mode && !replicating && is_write_command(cmd)) {
        return kvs_reply_error(out_buf, "-READONLY\r\n");
    }

    // 统一解析 key 和 value（仅用于普通命令，SETEX 等特殊命令内部会重新获取 value）
    char *key = (argc >= 2) ? argv[1] : NULL;
    char *value = (argc >= 3) ? argv[2] : NULL;

    int ret;
    int length;

    switch (cmd) {
#if ENABLE_ARRAY
    case KVS_CMD_SET:
        ret = kvs_array_set(&global_array, key, value, -1);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        // check_and_trigger_bgsave();
        break;

    case KVS_CMD_GET: {
        char *result = kvs_array_get(&global_array, key);
        if (result == NULL) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "$-1\r\n");
        } else {
            size_t vlen = strlen(result);
            length = snprintf(NULL, 0, "$%zu\r\n%s\r\n", vlen, result);
            *out_buf = kvs_malloc(length + 1);
            if (!*out_buf) return -2;
            sprintf(*out_buf, "$%zu\r\n%s\r\n", vlen, result);
        }
        break;
    }

    case KVS_CMD_DEL:
        ret = kvs_array_del(&global_array, key);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_MOD:
        ret = kvs_array_mod(&global_array, key, value);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        } else {
            *out_buf = kvs_malloc(20);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR no such key\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_EXIST:
        ret = kvs_array_exist(&global_array, key);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        break;
#endif // ENABLE_ARRAY

#if ENABLE_RBTREE
    case KVS_CMD_RSET:
        ret = kvs_rbtree_set(&global_rbtree, key, value, -1);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_RGET: {
        char *result = kvs_rbtree_get(&global_rbtree, key);
        if (result == NULL) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "$-1\r\n");
        } else {
            size_t vlen = strlen(result);
            length = snprintf(NULL, 0, "$%zu\r\n%s\r\n", vlen, result);
            *out_buf = kvs_malloc(length + 1);
            if (!*out_buf) return -2;
            sprintf(*out_buf, "$%zu\r\n%s\r\n", vlen, result);
        }
        break;
    }

    case KVS_CMD_RDEL:
        ret = kvs_rbtree_del(&global_rbtree, key);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_RMOD:
        ret = kvs_rbtree_mod(&global_rbtree, key, value);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        } else {
            *out_buf = kvs_malloc(20);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR no such key\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_REXIST:
        ret = kvs_rbtree_exist(&global_rbtree, key);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        break;
#endif // ENABLE_RBTREE

#if ENABLE_HASH
    case KVS_CMD_HSET:
        ret = kvs_hash_set(&global_hash, key, value, -1);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_HGET: {
        char *result = kvs_hash_get(&global_hash, key);
        if (result == NULL) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "$-1\r\n");
        } else {
            size_t vlen = strlen(result);
            length = snprintf(NULL, 0, "$%zu\r\n%s\r\n", vlen, result);
            *out_buf = kvs_malloc(length + 1);
            if (!*out_buf) return -2;
            sprintf(*out_buf, "$%zu\r\n%s\r\n", vlen, result);
        }
        
        break;
    }

    case KVS_CMD_HDEL:
        ret = kvs_hash_del(&global_hash, key);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_HMOD:
        ret = kvs_hash_mod(&global_hash, key, value);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        } else {
            *out_buf = kvs_malloc(20);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR no such key\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_HEXIST:
        ret = kvs_hash_exist(&global_hash, key);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        break;
#endif // ENABLE_HASH

#if ENABLE_SKIPLIST
    case KVS_CMD_SKSET:
        ret = kvs_skiplist_set(&global_skiplist, key, value, -1);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_SKGET: {
        char *result = kvs_skiplist_get(&global_skiplist, key);
        if (result == NULL) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "$-1\r\n");
        } else {
            size_t vlen = strlen(result);
            length = snprintf(NULL, 0, "$%zu\r\n%s\r\n", vlen, result);
            *out_buf = kvs_malloc(length + 1);
            if (!*out_buf) return -2;
            sprintf(*out_buf, "$%zu\r\n%s\r\n", vlen, result);
        }
        break;
    }

    case KVS_CMD_SKDEL:
        ret = kvs_skiplist_del(&global_skiplist, key);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_SKMOD:
        ret = kvs_skiplist_mod(&global_skiplist, key, value);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        } else {
            *out_buf = kvs_malloc(20);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR no such key\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;

    case KVS_CMD_SKEXIST:
        ret = kvs_skiplist_exist(&global_skiplist, key);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        }
        break;
#endif

    case KVS_CMD_BGSAVE:
        ret = kvs_persistence_bgsave();
        if (ret < 0) {
            *out_buf = kvs_malloc(22);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR bgsave failed\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        break;

    case KVS_CMD_SYNC:
    {
        struct sockaddr_in client_addr;
        if (kvs_get_client_addr(fd, &client_addr) != 0) {
            memset(&client_addr, 0, sizeof(client_addr));
        }
        // 解析复制端口参数，默认为客户端端口+1

        kvs_replication_master_handle_sync(fd, &client_addr);//调整状态，通知uprobe
        *out_buf = NULL;
        return 0;
    } break;
       


    case KVS_CMD_FINSYNC:
        kvs_replication_master_handle_finsync(fd);
        *out_buf = NULL;
        return 0;
        break;


    // ==================== 过期相关命令 ====================
#if ENABLE_ARRAY
    case KVS_CMD_SETEX: {   // SETEX key seconds value
        if (argc != 4) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[3]);
        char *real_value = argv[2];   // 真正的 value
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_array_set(&global_array, key, real_value, expire_ms);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_EXPIRE: {   // EXPIRE key seconds
        if (argc != 3) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[2]);
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_array_expire(&global_array, key, expire_ms);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else if (ret == 1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        } else {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_TTL: {      // TTL key
        if (argc != 2) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long ttl = kvs_array_ttl(&global_array, key);
        if (ttl == -2) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-2\r\n");
        } else if (ttl == -1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-1\r\n");
        } else {
            ttl = (ttl + 999) / 1000;
            int len = snprintf(NULL, 0, ":%lld\r\n", ttl);
            *out_buf = kvs_malloc(len + 1);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":%lld\r\n", ttl);
        }
        break;
    }
#endif
    // ==================== RBTree 过期命令 ====================
#if ENABLE_RBTREE
    case KVS_CMD_RSETEX: {   // RSETEX key seconds value
        if (argc != 4) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[3]);
        char *real_value = argv[2];
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_rbtree_set(&global_rbtree, key, real_value, expire_ms);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_REXPIRE: {   // REXPIRE key seconds
        if (argc != 3) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[2]);
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_rbtree_expire(&global_rbtree, key, expire_ms);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else if (ret == 1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        } else {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_RTTL: {      // RTTL key
        if (argc != 2) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long ttl = kvs_rbtree_ttl(&global_rbtree, key);
        if (ttl == -2) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-2\r\n");
        } else if (ttl == -1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-1\r\n");
        } else {
            ttl = (ttl + 999) / 1000;
            int len = snprintf(NULL, 0, ":%lld\r\n", ttl);
            *out_buf = kvs_malloc(len + 1);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":%lld\r\n", ttl);
        }
        break;
    }
#endif
    // ==================== Hash 过期命令 ====================
#if ENABLE_HASH
    case KVS_CMD_HSETEX: {   // HSETEX key seconds value
        if (argc != 4) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[3]);
        char *real_value = argv[2];
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_hash_set(&global_hash, key, real_value, expire_ms);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_HEXPIRE: {   // HEXPIRE key seconds
        if (argc != 3) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[2]);
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_hash_expire(&global_hash, key, expire_ms);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else if (ret == 1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        } else {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_HTTL: {      // HTTL key
        if (argc != 2) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long ttl = kvs_hash_ttl(&global_hash, key);
        if (ttl == -2) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-2\r\n");
        } else if (ttl == -1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-1\r\n");
        } else {
            ttl = (ttl + 999) / 1000;
            int len = snprintf(NULL, 0, ":%lld\r\n", ttl);
            *out_buf = kvs_malloc(len + 1);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":%lld\r\n", ttl);
        }
        break;
    }
#endif
    // ==================== SkipList 过期命令 ====================
#if ENABLE_SKIPLIST
    case KVS_CMD_SKSETEX: {   // SKSETEX key seconds value
        if (argc != 4) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[3]);
        char *real_value = argv[2];
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_skiplist_set(&global_skiplist, key, real_value, expire_ms);
        if (ret < 0) {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_SKEXPIRE: {   // SKEXPIRE key seconds
        if (argc != 3) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long seconds = atoll(argv[2]);
        long long expire_ms = kvs_current_time_ms() + seconds * 1000;
        ret = kvs_skiplist_expire(&global_skiplist, key, expire_ms);
        if (ret == 0) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":1\r\n");
        } else if (ret == 1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":0\r\n");
        } else {
            *out_buf = kvs_malloc(25);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR internal error\r\n");
        }
        persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        break;
    }

    case KVS_CMD_SKTTL: {      // SKTTL key
        if (argc != 2) {
            *out_buf = kvs_malloc(28);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }
        long long ttl = kvs_skiplist_ttl(&global_skiplist, key);
        if (ttl == -2) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-2\r\n");
        } else if (ttl == -1) {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":-1\r\n");
        } else {
            ttl = (ttl + 999) / 1000;
            int len = snprintf(NULL, 0, ":%lld\r\n", ttl);
            *out_buf = kvs_malloc(len + 1);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, ":%lld\r\n", ttl);
        }
        break;
    }
#endif
#if ENABLE_VECTOR
    case KVS_CMD_VSET: {
        /*
         * VSET key answer dim vector
         */
        if (argc != 5) {
            *out_buf = kvs_malloc(36);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
            break;
        }

        char *vkey = argv[1];
        char *answer = argv[2];
        int dim = atoi(argv[3]);
        char *vector_str = argv[4];

        if (dim <= 0) {
            *out_buf = kvs_malloc(26);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR invalid dim\r\n");
            break;
        }

        ret = kvs_vector_set(&global_vector_tree, vkey, answer, dim, vector_str);
        if (ret < 0) {
            *out_buf = kvs_malloc(30);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR vector set failed\r\n");
        } else {
            *out_buf = kvs_malloc(6);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "+OK\r\n");

            /*
             * 直接持久化 raw RESP。
             * 后面 AOF 重放时，只要 VSET 已注册，就能恢复。
             */
            persist_command_if_changed(cmd, ret, raw_msg, raw_len);
        }

        break;
    }

    case KVS_CMD_VGET: {
        /*
         * 两种用法：
         *
         * 1. 精确查询：
         *    VGET key
         *
         * 2. 向量查询：
         *    VGET dim vector threshold
         */

        if (argc == 2) {
            char *vkey = argv[1];

            vector_entry_t *entry = kvs_vector_get_by_key(&global_vector_tree, vkey);
            if (!entry) {
                *out_buf = kvs_malloc(5);
                if (!*out_buf) return -2;
                length = sprintf(*out_buf, "*0\r\n");
                break;
            }

            size_t key_len = strlen(entry->question);
            size_t answer_len = strlen(entry->answer);

            length = snprintf(
                NULL,
                0,
                "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                key_len, entry->question,
                answer_len, entry->answer
            );

            *out_buf = kvs_malloc(length + 1);
            if (!*out_buf) return -2;

            sprintf(
                *out_buf,
                "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                key_len, entry->question,
                answer_len, entry->answer
            );

            break;
        }

        if (argc == 4) {
            int dim = atoi(argv[1]);
            char *vector_str = argv[2];
            float threshold = (float)atof(argv[3]);

            if (dim <= 0) {
                *out_buf = kvs_malloc(26);
                if (!*out_buf) return -2;
                length = sprintf(*out_buf, "-ERR invalid dim\r\n");
                break;
            }

            vector_get_result_t result;
            memset(&result, 0, sizeof(result));

            ret = kvs_vector_get_by_vector(
                &global_vector_tree,
                dim,
                vector_str,
                threshold,
                &result
            );

            if (ret == 1) {
                char score_buf[64];
                snprintf(score_buf, sizeof(score_buf), "%.6f", result.score);

                size_t key_len = strlen(result.question);
                size_t answer_len = strlen(result.answer);
                size_t score_len = strlen(score_buf);

                length = snprintf(
                    NULL,
                    0,
                    "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                    key_len, result.question,
                    answer_len, result.answer,
                    score_len, score_buf
                );

                *out_buf = kvs_malloc(length + 1);
                if (!*out_buf) return -2;

                sprintf(
                    *out_buf,
                    "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                    key_len, result.question,
                    answer_len, result.answer,
                    score_len, score_buf
                );

                break;
            }

            if (ret == 0) {
                *out_buf = kvs_malloc(5);
                if (!*out_buf) return -2;
                length = sprintf(*out_buf, "*0\r\n");
                break;
            }

            *out_buf = kvs_malloc(31);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR vector get failed\r\n");
            break;
        }

        *out_buf = kvs_malloc(36);
        if (!*out_buf) return -2;
        length = sprintf(*out_buf, "-ERR wrong number of arguments\r\n");
        break;
    }

    case KVS_CMD_VSEARCH: {
        /*
         * VSEARCH dim vector topk threshold
         * 返回：*N，每个元素是 [question, answer, score]
         */
        int dim = atoi(argv[1]);
        char *vector_str = argv[2];
        int topk = atoi(argv[3]);
        float threshold = (float)atof(argv[4]);
        vector_search_result_t *results = NULL;
        int result_count = 0;

        if (dim <= 0 || topk <= 0) {
            length = kvs_reply_error(out_buf, "-ERR invalid vector search args\r\n");
            if (length < 0) return length;
            break;
        }

        ret = kvs_vector_search(&global_vector_tree,
                                dim,
                                vector_str,
                                topk,
                                threshold,
                                &results,
                                &result_count);
        if (ret < 0) {
            *out_buf = kvs_malloc(34);
            if (!*out_buf) return -2;
            length = sprintf(*out_buf, "-ERR vector search failed\r\n");
            break;
        }

        length = snprintf(NULL, 0, "*%d\r\n", result_count);
        for (int i = 0; i < result_count; i++) {
            char score_buf[64];
            size_t key_len = strlen(results[i].question);
            size_t answer_len = strlen(results[i].answer);
            snprintf(score_buf, sizeof(score_buf), "%.6f", results[i].score);
            size_t score_len = strlen(score_buf);

            length += snprintf(NULL,
                               0,
                               "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                               key_len,
                               results[i].question,
                               answer_len,
                               results[i].answer,
                               score_len,
                               score_buf);
        }

        *out_buf = kvs_malloc(length + 1);
        if (!*out_buf) {
            kvs_vector_search_results_free(results);
            return -2;
        }

        int offset = sprintf(*out_buf, "*%d\r\n", result_count);
        for (int i = 0; i < result_count; i++) {
            char score_buf[64];
            size_t key_len = strlen(results[i].question);
            size_t answer_len = strlen(results[i].answer);
            snprintf(score_buf, sizeof(score_buf), "%.6f", results[i].score);
            size_t score_len = strlen(score_buf);

            offset += sprintf(*out_buf + offset,
                              "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                              key_len,
                              results[i].question,
                              answer_len,
                              results[i].answer,
                              score_len,
                              score_buf);
        }

        kvs_vector_search_results_free(results);
        break;
    }
#endif

    case KVS_CMD_PING:
        *out_buf = kvs_malloc(7);
        if (!*out_buf) return -2;
        // kvs_persistence_append(raw_msg, raw_len);
        length = sprintf(*out_buf, "+PONG\r\n");
        break;

    default:
        *out_buf = kvs_malloc(23);
        if (!*out_buf) return -2;
        length = sprintf(*out_buf, "-ERR unknown command\r\n");
        break;
    }

    return length;
}
