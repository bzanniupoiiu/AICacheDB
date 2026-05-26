// test_repl_perf.c - 主从同步性能对比测试
// 编译: gcc -O2 test_repl_perf.c -o test_repl_perf -lpthread
// 用法: ./test_repl_perf <master_ip> <master_port> <enable_slave> <duration_sec>
// 示例: ./test_repl_perf 192.168.32.128 8888 1 10

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_CONNS 32
#define BATCH_SIZE 100          // Pipeline批量大小
#define KEY_PREFIX "perf_test_key_"

static int g_running = 1;
static long long g_total_ops = 0;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char ip[16];
    int port;
    int thread_id;
} thread_arg_t;

// 发送RESP格式的SET命令（带pipeline）
static int send_set_pipeline(int fd, int count, long long start_id, char *resp_buf, int *resp_len) {
    // 构建多个SET命令拼接
    char *buf = malloc(1024 * count);
    int off = 0;
    for (int i = 0; i < count; i++) {
        off += sprintf(buf + off, "*3\r\n$3\r\nSET\r\n$%d\r\n%s%lld\r\n$5\r\nvalue\r\n",
                       (int)(strlen(KEY_PREFIX) + 20), KEY_PREFIX, start_id + i);
    }
    // 发送
    ssize_t n = send(fd, buf, off, 0);
    free(buf);
    if (n != off) return -1;
    
    // 接收所有响应
    int total_recv = 0;
    while (total_recv < count) {
        ssize_t nr = recv(fd, resp_buf + total_recv, *resp_len - total_recv, 0);
        if (nr <= 0) return -1;
        total_recv += nr;
    }
    // 简单检查每个响应是否为"+OK\r\n"
    char *p = resp_buf;
    for (int i = 0; i < count; i++) {
        if (strncmp(p, "+OK\r\n", 5) != 0) return -1;
        p += 5;
    }
    *resp_len = total_recv;
    return 0;
}

static void *worker_thread(void *arg) {
    thread_arg_t *ta = (thread_arg_t*)arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return NULL; }
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ta->port);
    inet_pton(AF_INET, ta->ip, &addr.sin_addr);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return NULL;
    }
    
    char *resp_buf = malloc(1024 * BATCH_SIZE);
    int resp_len = 1024 * BATCH_SIZE;
    long long local_ops = 0;
    long long key_id = ta->thread_id * 10000000LL;
    
    while (g_running) {
        if (send_set_pipeline(fd, BATCH_SIZE, key_id, resp_buf, &resp_len) != 0) break;
        local_ops += BATCH_SIZE;
        key_id += BATCH_SIZE;
    }
    
    free(resp_buf);
    close(fd);
    
    pthread_mutex_lock(&g_mutex);
    g_total_ops += local_ops;
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static double run_test(const char *ip, int port, int duration_sec, int threads) {
    g_total_ops = 0;
    g_running = 1;
    
    pthread_t *tids = malloc(sizeof(pthread_t) * threads);
    thread_arg_t *args = malloc(sizeof(thread_arg_t) * threads);
    
    for (int i = 0; i < threads; i++) {
        strcpy(args[i].ip, ip);
        args[i].port = port;
        args[i].thread_id = i;
        pthread_create(&tids[i], NULL, worker_thread, &args[i]);
    }
    
    sleep(duration_sec);
    g_running = 0;
    
    for (int i = 0; i < threads; i++) {
        pthread_join(tids[i], NULL);
    }
    
    free(tids);
    free(args);
    return (double)g_total_ops / duration_sec;
}

int main(int argc, char *argv[]) {
    if (argc < 5) {
        fprintf(stderr, "Usage: %s <master_ip> <master_port> <enable_slave> <duration_sec>\n", argv[0]);
        return 1;
    }
    const char *master_ip = argv[1];
    int master_port = atoi(argv[2]);
    int enable_slave = atoi(argv[3]);
    int duration = atoi(argv[4]);
    int threads = 1;
    
    printf("Testing with slave %s, duration %d seconds, threads %d\n",
           enable_slave ? "ON" : "OFF", duration, threads);
    
    double qps = run_test(master_ip, master_port, duration, threads);
    printf("Result: %.2f QPS\n", qps);
    return 0;
}