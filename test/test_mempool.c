#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>

#define MAX_MSG 1024
#define TOTAL_OPS 1000000   // 100万次操作

int connect_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = inet_addr(ip)
    };
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }
    return fd;
}

void send_cmd(int fd, const char *cmd) {
    send(fd, cmd, strlen(cmd), 0);
    char resp[MAX_MSG];
    recv(fd, resp, sizeof(resp), 0);
    // 忽略响应，只关心吞吐量
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    int fd = connect_server(ip, port);
    if (fd < 0) return 1;

    // ========== 插入100万条数据 ==========
    printf("开始插入 %d 条数据到服务器...\n", TOTAL_OPS);
    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 0; i < TOTAL_OPS; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(val, sizeof(val), "value%d", i);
        int key_len = strlen(key);
        int val_len = strlen(val);
        // 构造SKSET命令: *3\r\n$5\r\nSKSET\r\n$key_len\r\nkey\r\n$val_len\r\nval\r\n
        char cmd[256];
        int pos = snprintf(cmd, sizeof(cmd), "*3\r\n$5\r\nSKSET\r\n$%d\r\n%s\r\n$%d\r\n%s\r\n", 
                           key_len, key, val_len, val);
        send_cmd(fd, cmd);
        
        if ((i + 1) % 100000 == 0) {
            printf("已插入 %d 条数据\n", i + 1);
        }
    }

    gettimeofday(&end, NULL);
    double insert_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double insert_qps = TOTAL_OPS / insert_time;
    printf("\n插入完成！\n");
    printf("插入耗时: %.2f 秒, 插入QPS: %.2f\n\n", insert_time, insert_qps);

    // ========== 等待用户按键开始删除 ==========
    printf("按 Enter 键开始删除这 %d 条数据...\n", TOTAL_OPS);
    getchar();

    // ========== 删除100万条数据 ==========
    printf("开始删除 %d 条数据...\n", TOTAL_OPS);
    gettimeofday(&start, NULL);

    for (int i = 0; i < TOTAL_OPS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        int key_len = strlen(key);
        // 构造SKDEL命令: *2\r\n$5\r\nSKDEL\r\n$key_len\r\nkey\r\n
        char cmd[256];
        int pos = snprintf(cmd, sizeof(cmd), "*2\r\n$5\r\nSKDEL\r\n$%d\r\n%s\r\n", 
                           key_len, key);
        send_cmd(fd, cmd);
        
        if ((i + 1) % 100000 == 0) {
            printf("已删除 %d 条数据\n", i + 1);
        }
    }

    gettimeofday(&end, NULL);
    double delete_time = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double delete_qps = TOTAL_OPS / delete_time;

    close(fd);

    // ========== 输出统计结果 ==========
    printf("\n========== 测试结果 ==========\n");
    printf("操作类型: 跳表引擎 (SKSET/SKDEL)\n");
    printf("数据量: %d 条\n", TOTAL_OPS);
    printf("插入耗时: %.2f 秒, 插入QPS: %.2f\n", insert_time, insert_qps);
    printf("删除耗时: %.2f 秒, 删除QPS: %.2f\n", delete_time, delete_qps);
    printf("总耗时: %.2f 秒\n", insert_time + delete_time);
    printf("==============================\n");

    return 0;
}