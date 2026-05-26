


#ifndef __KV_STORE_H__
#define __KV_STORE_H__


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <netinet/in.h>   
#include <sys/socket.h>  
#include <pthread.h>
#include <sys/time.h>
#include "server.h"

#define NETWORK_REACTOR 	0
#define NETWORK_PROACTOR	1
#define NETWORK_NTYCO		2
#define NETWORK_SELECT		NETWORK_REACTOR


#define ENABLE_ARRAY		1
#define ENABLE_RBTREE		1
#define ENABLE_HASH			1
#define ENABLE_SKIPLIST		1
#define ENABLE_VECTOR 		1




typedef int (*msg_handler)(int fd, char *msg, int length,  char **out_buf);
// __________________kvstore.c里面的_______________________
extern int reactor_start(unsigned short port);
extern int proactor_start(unsigned short port);
extern int ntyco_start(unsigned short port);

void *kvs_malloc(size_t size);
void kvs_free(void *ptr);
int kvs_protocol(int fd, const char *msg, size_t length, char **out_buf);
int kvs_execute_command(int fd, int argc, char **argv,
                        const char *raw_msg, int raw_len,
                        char **out_buf);
int resp_parse_one_command(const char *buf, size_t len, size_t *cmd_len) ;

// _______________________数组_______________________
#if ENABLE_ARRAY
typedef struct kvs_array_item_s {
	char *key;
	char *value;
	long long expire_ms;   // 过期时间戳（毫秒），-1 表示永不过期
} kvs_array_item_t;
#define KVS_ARRAY_SIZE		1024*1024
typedef struct kvs_array_s {
	kvs_array_item_t *table;
	int total;
} kvs_array_t;
int kvs_array_create(kvs_array_t *inst);
void kvs_array_destory(kvs_array_t *inst);
int kvs_array_set(kvs_array_t *inst, char *key, char *value, long long expire_ms);
char* kvs_array_get(kvs_array_t *inst, char *key);
int kvs_array_del(kvs_array_t *inst, char *key);
int kvs_array_mod(kvs_array_t *inst, char *key, char *value);
int kvs_array_exist(kvs_array_t *inst, char *key);
//过期函数
int kvs_array_expire(kvs_array_t *inst, char *key, long long expire_ms);
long long kvs_array_ttl(kvs_array_t *inst, char *key);
void kvs_array_scan_expired(kvs_array_t *inst);
#endif
// _______________________红黑树_______________________
#if ENABLE_RBTREE
#define RED				1
#define BLACK 			2
#define ENABLE_KEY_CHAR		1
#if ENABLE_KEY_CHAR
typedef char* KEY_TYPE;
#else
typedef int KEY_TYPE; // key
#endif
typedef struct _rbtree_node {
	unsigned char color;
	struct _rbtree_node *right;
	struct _rbtree_node *left;
	struct _rbtree_node *parent;
	KEY_TYPE key;
	void *value;
	long long expire_ms;
} rbtree_node;

typedef struct _rbtree {
	rbtree_node *root;
	rbtree_node *nil;
} rbtree;



typedef struct _rbtree kvs_rbtree_t;
int kvs_rbtree_create(kvs_rbtree_t *inst);
void kvs_rbtree_destory(kvs_rbtree_t *inst);
int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value, long long expire_ms);
char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_del(kvs_rbtree_t *inst, char *key);
int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value);
int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key);
//过期函数
int kvs_rbtree_expire(kvs_rbtree_t *inst, char *key, long long expire_ms);
long long kvs_rbtree_ttl(kvs_rbtree_t *inst, char *key);
void kvs_rbtree_scan_expired(kvs_rbtree_t *inst);
#endif


// _______________________哈希_______________________

#if ENABLE_HASH
#define MAX_TABLE_SIZE	1024
#define ENABLE_KEY_POINTER	1
typedef struct hashnode_s {
#if ENABLE_KEY_POINTER
	char *key;
	char *value;
#else
	char key[MAX_KEY_LEN];
	char value[MAX_VALUE_LEN];
#endif
	struct hashnode_s *next;
	long long expire_ms;
} hashnode_t;
typedef struct hashtable_s {
	hashnode_t **nodes; //* change **, 
	int max_slots;
	int count;

} hashtable_t;
typedef struct hashtable_s kvs_hash_t;
int kvs_hash_create(kvs_hash_t *hash);
void kvs_hash_destory(kvs_hash_t *hash);
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value, long long expire_ms);
char * kvs_hash_get(kvs_hash_t *hash, char *key);
int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value);
int kvs_hash_del(kvs_hash_t *hash, char *key);
int kvs_hash_exist(kvs_hash_t *hash, char *key);
//过期函数
int kvs_hash_expire(kvs_hash_t *inst, char *key, long long expire_ms);
long long kvs_hash_ttl(kvs_hash_t *inst, char *key);
void kvs_hash_scan_expired(kvs_hash_t *inst);
#endif
// _______________________跳表_______________________
#if ENABLE_SKIPLIST
#define SKIPLIST_MAX_LEVEL 16   // 
#define SKIPLIST_P 0.25          // ӣ
typedef struct skiplist_node_s {
    char *key;
    char *value;
    struct skiplist_node_s **forward; // 
	long long expire_ms;
} skiplist_node_t;
typedef struct kvs_skiplist_s {
    skiplist_node_t *header;      // 
    int level;                     // 
    int size;                       //
} kvs_skiplist_t;
int kvs_skiplist_create(kvs_skiplist_t *inst);
void kvs_skiplist_destroy(kvs_skiplist_t *inst);
int kvs_skiplist_set(kvs_skiplist_t *inst, char *key, char *value, long long expire_ms);
char *kvs_skiplist_get(kvs_skiplist_t *inst, char *key);
int kvs_skiplist_del(kvs_skiplist_t *inst, char *key);
int kvs_skiplist_mod(kvs_skiplist_t *inst, char *key, char *value);
int kvs_skiplist_exist(kvs_skiplist_t *inst, char *key);
//过期函数
int kvs_skiplist_expire(kvs_skiplist_t *inst, char *key, long long expire_ms);
long long kvs_skiplist_ttl(kvs_skiplist_t *inst, char *key);
void kvs_skiplist_scan_expired(kvs_skiplist_t *inst);
#endif

// _______________________向量数据库________________________________
//红黑树存储引擎给向量数据库保留的接口
rbtree_node *rbtree_mini(rbtree *T, rbtree_node *x);
rbtree_node *rbtree_search(rbtree *T,KEY_TYPE key);
void rbtree_insert(rbtree *T,rbtree_node *z);
rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z);

#if ENABLE_VECTOR

typedef struct vectory_entry
{
	char *question;
	char *answer;
	float *vector;
	int dim;
	float norm;
	long long ctime;
}vector_entry_t;

typedef struct kvs_vector
{
	kvs_rbtree_t tree;
	int count;
}kvs_vector_t;

typedef struct vector_get_result
{
	char *question;
	char *answer;
	float score;

}vector_get_result_t;

int kvs_vector_create(kvs_vector_t *inst);
void kvs_vector_destroy(kvs_vector_t *inst);

int kvs_vector_set(kvs_vector_t *inst, char *question, char *answer, int dim, char *vector_str);
vector_entry_t *kvs_vector_get_by_key(kvs_vector_t *inst, char *question);

int kvs_vector_get_by_vector(kvs_vector_t *inst, int dim, char *vector_str, float threshold, vector_get_result_t *result);
#endif
// _______________________持久化————————————————————————————————

#define AOF_RING_CAPACITY  (1024 * 1024 * 16)   // 无锁环形缓冲区槽位数，可根据内存调整

int kvs_persistence_init();
//
void kvs_persistence_close();
//
int kvs_persistence_load();
//ִ
int kvs_persistence_bgsave();
void kvs_check_auto_bgsave(void);
//
int kvs_persistence_append(const char *cmd,int len);
//
int kvs_persistence_is_loading();
//

int kvs_persistence_get_eventfd(void);
void kvs_persistence_process_completions(void) ;

int rdb_load_files(void);


// _______________________内存池————————————————————————————————
typedef struct mp_node_s {
    unsigned char *last;
    unsigned char *end;
    struct mp_node_s *next;
} mp_node_t;

typedef struct mp_large_s {
    struct mp_large_s *next;
    void *alloc;
} mp_large_t;

typedef struct mp_pool_s {
    size_t max;
    struct mp_node_s *head;
    struct mp_large_s *large;
} mp_pool_t;

int kvs_mempool_create(mp_pool_t *m, int block_size);
void kvs_mempool_destroy(mp_pool_t *m);
void *kvs_mempool_alloc(mp_pool_t *m, size_t size);
void kvs_mempool_free(mp_pool_t *m, void *ptr);

// _______________________主从复制————————————————————————————————

typedef enum {//从机的状态
	KVS_REPL_STATE_INIT = 0,        // 显式定义初始状态
    KVS_REPL_STATE_WAIT_RDMA=1,   // 等待 RDMA 连接
    KVS_REPL_STATE_WAIT_FINSYNC=2,// 等待 FINSYNC
	KVS_REPL_STATE_ONLINE=3       // 正常同步中
} replica_state_t;

// 从节点结构体（主节点使用）
typedef struct kvs_replica_s {
    int tcp_fd;                     // TCP 连接描述符
    struct sockaddr_in tcp_addr;    // TCP 地址
    replica_state_t state;
    char *cmd_queue;                // 增量命令队列（连续内存）
    size_t cmd_queue_size;          // 队列已用大小
    size_t cmd_queue_cap;           // 队列容量
    pthread_mutex_t queue_mutex;    // 队列锁
    struct rdma_cm_id *rdma_cm_id;  // RDMA 连接 ID（建立后）
    struct kvs_replica_s *next;

	// 用于 RDB 发送的状态
    int rdb_fd;                 // RDB 文件描述符
    off_t rdb_size;             // 文件总大小
    off_t rdb_offset;           // 已发送偏移
} kvs_replica_t;

// 主节点全局从链表
extern kvs_replica_t *g_replicas;


// 主从复制 API（主节点）
int kvs_replication_master_init(void);
void kvs_replication_master_cleanup(void);
// void kvs_replication_feed_slaves(const char *cmd, int len);
void kvs_replication_master_handle_sync(int tcp_fd, struct sockaddr_in *addr);
void kvs_replication_master_handle_finsync(int tcp_fd);
// void kvs_replication_master_send_queued_commands(kvs_replica_t *replica);

// 从节点 API
int kvs_replication_slave_start(const char *master_host, int master_port, int rdma_port);
void kvs_replication_slave_cleanup(void);

// 全局标志
extern int replicating;   // 从节点是否正在从主节点接收命令（避免循环）
// extern int bgsave_running; // 主节点是否正在 BGSAVE
// extern pid_t bgsave_pid;
int rdb_save_to_file(const char *filename);   // 保存 RDB 到指定文件



// _______________________config————————————————————————————————
typedef enum{
	PERSIST_NONE,
	PERSIST_RDB,
	PERSIST_AOF,
	PERSIST_MIXED
}persist_mode_t;

typedef struct {
	char bind_ip[16];
	int ports[16];
	int port_count;

	persist_mode_t persist_mode;
	int rdb_save_params[10][2];
	int rdb_save_count;

	int appendonly;  // 0/1/2
	int appendfsync;  // 0=no,1=everysec,2=always

	int slave_mode;
	char master_host[64];
	int master_port;

	size_t mempool_size;

}kvs_config_t;	

extern kvs_config_t g_config;

int load_config(const char *filename);
// _______________________expire————————————————————————————————
static inline long long kvs_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void kvs_expire_scan(void);   // 添加此声明
int kvs_start_expire_scanner(void);
void kvs_stop_expire_scanner(void);
#endif