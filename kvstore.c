#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include "kvstore.h"
#include <signal.h>

void kvs_signal_init() { signal(SIGCHLD, SIG_IGN); }


message_callback g_network_callback = NULL;//回调函数


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

//---------------------------------------------------
int replicating = 0;//是否在复制（全局变量）

//--------------------------------------------------
static mp_pool_t g_mempool;   
#if 0
void *kvs_malloc(size_t size) { return kvs_mempool_alloc(&g_mempool, size); }
void kvs_free(void *ptr) { kvs_mempool_free(&g_mempool, ptr); }
#else
void* kvs_malloc(size_t size) { return malloc(size); }
void kvs_free(void *ptr) { free(ptr); }
#endif

//解析一条指令，看看是不是resp
int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len) {
    if (len < 1 || buf[0] != '*') return -1;
    const char *p = buf + 1;
    int num = 0;
    while (p < buf + len && *p >= '0' && *p <= '9') {
        num = num * 10 + (*p - '0');
        p++;
    }
    if (p >= buf + len || *p != '\r') return 0;
    p++; if (p >= buf + len || *p != '\n') return 0;
    p++;

    for (int i = 0; i < num; i++) {
        if (p >= buf + len || *p != '$') return -1;
        p++;
        int bulk_len = 0;
        while (p < buf + len && *p >= '0' && *p <= '9') {
            bulk_len = bulk_len * 10 + (*p - '0');
            p++;
        }
        if (p >= buf + len || *p != '\r') return 0;
        p++; if (p >= buf + len || *p != '\n') return 0;
        p++;
        if (p + bulk_len > buf + len) return 0;
        p += bulk_len;
        if (p + 1 >= buf + len || *p != '\r') return 0;
        p++; if (p >= buf + len || *p != '\n') return 0;
        p++;
    }
    *cmd_len = p - buf;
    return 1;
}
//拆分resp
char **resp_parse_arguments(const char *buf, size_t len, int *argc)
{
    if (len < 1 || buf[0] != '*') return NULL;

    const char *p = buf + 1;
    int num = 0;

    while (p < buf + len && *p >= '0' && *p <= '9') {//ַתnumargcĸ
        num = num * 10 + (*p - '0');
        p++;
    }
    //  \r\n
    if (p >= buf + len || *p != '\r') return NULL;
    p++;
    if (p >= buf + len || *p != '\n') return NULL;
    p++;

    if (num <= 0) return NULL;

    //*3\r\n$3\r\nSET\r\n$2\r\nK1\r\n$2\r\nV1\r\n

    char **argv = (char **)kvs_malloc(sizeof(char *) * num);
    if (!argv) return NULL;

    int i;
    for (i = 0; i < num; i++) {

        if (p >= buf + len || *p != '$')
            goto err;
        p++;

        int bulk_len = 0;
        while (p < buf + len && *p >= '0' && *p <= '9') {
            bulk_len = bulk_len * 10 + (*p - '0');//ַת
            p++;
        }

        if (p >= buf + len || *p != '\r') goto err;
        p++;
        if (p >= buf + len || *p != '\n') goto err;
        p++;

        if (p + bulk_len > buf + len) goto err;

        argv[i] = (char *)kvs_malloc(bulk_len + 1);
        if (!argv[i]) goto err;

        memcpy(argv[i], p, bulk_len);
        argv[i][bulk_len] = '\0';

        p += bulk_len;

        if (p >= buf + len || *p != '\r') goto err;
        p++;
        if (p >= buf + len || *p != '\n') goto err;
        p++;
    }

    *argc = num;
    return argv;

err:
    for (int j = 0; j < i; j++)
        kvs_free(argv[j]);

    kvs_free(argv);
    return NULL;
}
//执行一条resp，协议解析
int kvs_protocol(int fd, const char *msg, size_t length, char **out_buf) {
    //  RESP 执行
   
    int argc;
    char **argv = resp_parse_arguments(msg, length, &argc);//拆分成不同的字符串
    if (!argv) {//错误判断
        *out_buf = kvs_malloc(22);
        if (!*out_buf) return -2;
        strcpy(*out_buf, "-ERR protocol error\r\n");
        return 21;
    }

    int resp_len = kvs_execute_command(fd, argc, argv, msg, length, out_buf);

    for (int i = 0; i < argc; i++) kvs_free(argv[i]);
    kvs_free(argv);

    return resp_len;
}

//初始化============================================================
int init_kvengine(void) {
		
	if (kvs_mempool_create(&g_mempool, 256 * 1024 * 1024) != 0) {//256MB
		return -1;
	}

	#if ENABLE_ARRAY
    memset(&global_array, 0, sizeof(kvs_array_t));
    if (kvs_array_create(&global_array) != 0) {
        fprintf(stderr, "Failed to create array engine\n");
        return -1;
    }
    #endif

    #if ENABLE_RBTREE
        memset(&global_rbtree, 0, sizeof(kvs_rbtree_t));
        if (kvs_rbtree_create(&global_rbtree) != 0) {
            fprintf(stderr, "Failed to create rbtree engine\n");
            return -1;
        }
    #endif

    #if ENABLE_HASH
        memset(&global_hash, 0, sizeof(kvs_hash_t));
        if (kvs_hash_create(&global_hash) != 0) {
            fprintf(stderr, "Failed to create hash engine\n");
            return -1;
        }
    #endif

    #if ENABLE_SKIPLIST
        memset(&global_skiplist, 0, sizeof(kvs_skiplist_t));
        if (kvs_skiplist_create(&global_skiplist) != 0) {
            fprintf(stderr, "Failed to create skiplist engine\n");
            return -1;
        }
    #endif

    #if ENABLE_VECTOR
        memset(&global_vector_tree, 0, sizeof(kvs_vector_t));
        if (kvs_vector_create(&global_vector_tree) != 0) {
            fprintf(stderr, "Failed to create vector rbtree engine\n");
            return -1;
        }
    #endif

	return 0;
}

void dest_kvengine(void) {
	#if ENABLE_ARRAY
		kvs_array_destory(&global_array);
	#endif
	#if ENABLE_RBTREE
		kvs_rbtree_destory(&global_rbtree);
	#endif
	#if ENABLE_HASH
		kvs_hash_destory(&global_hash);
	#endif
	#if ENABLE_SKIPLIST
		kvs_skiplist_destroy(&global_skiplist);
	#endif
    #if ENABLE_VECTOR
        kvs_vector_destroy(&global_vector_tree);
    #endif

	kvs_persistence_close();     // AOF

	kvs_mempool_destroy(&g_mempool);//

}



int main(int argc, char *argv[]) {
    kvs_signal_init();

    g_network_callback = kvs_protocol; //设置网络层读回调

    //配置文件--------------------------------------------------------------
    const char *config_file = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:")) != -1) {
        if (opt == 'c') config_file = optarg;
    }
    // 加载配置文件（如果提供）
    if (config_file) {
        if (load_config(config_file) != 0) {
            fprintf(stderr, "Failed to load config file: %s\n", config_file);
            return -1;
        }
    } else {//必须使用配置文件
        fprintf(stderr, "Failed to load config file!!!\n");
        return -1;
    }


    //初始化存储引擎和内存池-------------------------------------------------
    init_kvengine();

    
    // 持久化初始化（可选检查）----------------------------------------------
    if (kvs_persistence_init() != 0) {//初始化always还是no模式
        fprintf(stderr, "Failed to init persistence\n");
        return -1;
    }  
    //加载aof--------------------------------------------------------------
    kvs_persistence_load();      //  rdb模式就加载rdb文件，aof模式就加载aof文件

    
    //创建子线程扫描过期键值对------------------------------------------------
    // if (kvs_start_expire_scanner() != 0) {
    //     fprintf(stderr, "Failed to start expire scanner thread\n");
    //     dest_kvengine();
    //     return -1;
    // }


    if (g_config.slave_mode) {//如果是从机模式
        if (kvs_replication_slave_start(g_config.master_host, g_config.master_port, 2000) != 0) {
            fprintf(stderr, "Failed to start slave replication\n");
            dest_kvengine();
            return -1;
        }
    printf("[slave] KVStore is listening on port: %d!!!\n", g_config.ports[0]);

    } else {//如果是主机模式
        if (kvs_replication_master_init() != 0) {
            fprintf(stderr, "Failed to init master replication\n");
            dest_kvengine();
            return -1;
        }
    printf("[master] KVStore is listening on port: %d!!!\n", g_config.ports[0]);

    }


	//启动网络
    #if (NETWORK_SELECT == NETWORK_REACTOR)
            reactor_start(g_config.ports[0]);
    #elif (NETWORK_SELECT == NETWORK_NTYCO)
            ntyco_start(g_config.ports[0]);
    #elif (NETWORK_SELECT == NETWORK_PROACTOR)
            proactor_start(g_config.ports[0]);
    #endif
// -----------------------------------------------------------
    dest_kvengine();
    return 0;

}


