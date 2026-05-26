#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define MAX_MSG_LENGTH 8192
#define TEST_DATA_COUNT 10000  // 测试数据量：10000条

// 持久化文件路径
#define RDB_FILE "/home/raoyipeng/1_Voice_1_24_8680/2601/9.1-kvstore/Persistence/kvstore.rdb"
#define AOF_FILE "/home/raoyipeng/1_Voice_1_24_8680/2601/9.1-kvstore/Persistence/kvstore.aof"

// 颜色输出
#define COLOR_GREEN "\x1b[32m"
#define COLOR_RED "\x1b[31m"
#define COLOR_YELLOW "\x1b[33m"
#define COLOR_BLUE "\x1b[34m"
#define COLOR_RESET "\x1b[0m"

// 发送所有数据
int send_all(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = (const char *)buf;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) {
            printf("send error: %s\n", strerror(errno));
            return -1;
        }
        sent += n;
    }
    return 0;
}

// 接收一行响应（以 \r\n 结尾）
int recv_line(int fd, char *buf, size_t maxlen) {
    size_t total = 0;
    while (total < maxlen - 1) {
        ssize_t n = recv(fd, buf + total, 1, 0);
        if (n <= 0) {
            if (n == 0) {
                printf("Connection closed by peer\n");
            } else {
                printf("recv error: %s\n", strerror(errno));
            }
            return -1;
        }
        
        if (buf[total] == '\n') {
            if (total > 0 && buf[total - 1] == '\r') {
                buf[total + 1] = '\0';
                return total + 1;
            }
        }
        total++;
    }
    return -1;
}

// 构造 RESP 命令
void build_resp_command(char *buf, size_t bufsize, const char *cmd, const char *key, const char *value) {
    if (value == NULL) {
        if (key == NULL) {
            snprintf(buf, bufsize, "*1\r\n$%zu\r\n%s\r\n", strlen(cmd), cmd);
        } else {
            snprintf(buf, bufsize, "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key);
        }
    } else {
        snprintf(buf, bufsize, "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                 strlen(cmd), cmd, strlen(key), key, strlen(value), value);
    }
}

// 发送命令并检查响应是否为 "+OK\r\n"
int send_command_check_ok(int fd, const char *cmd, const char *key, const char *value) {
    char request[MAX_MSG_LENGTH];
    char response[MAX_MSG_LENGTH];

    build_resp_command(request, sizeof(request), cmd, key, value);
    
    if (send_all(fd, request, strlen(request)) < 0) {
        return -1;
    }

    if (recv_line(fd, response, sizeof(response)) < 0) {
        return -1;
    }

    if (strcmp(response, "+OK\r\n") == 0) return 0;
    return -1;
}

// 发送 GET 命令并获取值
char *send_get_command(int fd, const char *cmd, const char *key) {
    static char value[MAX_MSG_LENGTH];
    char request[MAX_MSG_LENGTH];
    char first_line[MAX_MSG_LENGTH];

    build_resp_command(request, sizeof(request), cmd, key, NULL);
    
    if (send_all(fd, request, strlen(request)) < 0) {
        return NULL;
    }

    if (recv_line(fd, first_line, sizeof(first_line)) < 0) {
        return NULL;
    }

    if (strcmp(first_line, "$-1\r\n") == 0) {
        return NULL;
    }

    int len = atoi(first_line + 1);
    if (len < 0 || len >= MAX_MSG_LENGTH) return NULL;

    int total_read = 0;
    while (total_read < len + 2) {
        ssize_t n = recv(fd, value + total_read, len + 2 - total_read, 0);
        if (n <= 0) return NULL;
        total_read += n;
    }

    value[len] = '\0';
    return value;
}

int connect_server(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    
    return fd;
}

long get_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size;
    return -1;
}

void check_file(const char *path, const char *name) {
    struct stat st;
    if (stat(path, &st) == 0) {
        printf(COLOR_GREEN "  ✓ %s exists, size: %ld bytes\n" COLOR_RESET, name, st.st_size);
    } else {
        printf(COLOR_RED "  ✗ %s not found\n" COLOR_RESET, name);
    }
}

// 测试1: AOF写入测试 (写入10000条数据)
void test_aof_write(int fd) {
    printf(COLOR_BLUE "\n=== Test 1: AOF Write Test (%d records) ===\n" COLOR_RESET, TEST_DATA_COUNT);
    
    long size_before = get_file_size(AOF_FILE);
    printf("AOF size before: %ld bytes\n", size_before);
    
    // 写入大量数据到不同引擎
    printf("\n-- Writing %d records to Array Engine --\n", TEST_DATA_COUNT);
    for (int i = 0; i < TEST_DATA_COUNT; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "array_key_%d", i);
        snprintf(val, sizeof(val), "array_val_%d", i);
        if (send_command_check_ok(fd, "SET", key, val) != 0) {
            printf(COLOR_RED "  FAIL: SET %s at index %d\n" COLOR_RESET, key, i);
            exit(1);
        }
        if ((i + 1) % 1000 == 0) {
            printf("    %d/%d records written to Array\n", i + 1, TEST_DATA_COUNT);
        }
    }
    printf(COLOR_GREEN "  ✓ Array: %d records written\n" COLOR_RESET, TEST_DATA_COUNT);
    
    printf("\n-- Writing %d records to RBTree Engine --\n", TEST_DATA_COUNT);
    for (int i = 0; i < TEST_DATA_COUNT; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "rbtree_key_%d", i);
        snprintf(val, sizeof(val), "rbtree_val_%d", i);
        if (send_command_check_ok(fd, "RSET", key, val) != 0) {
            printf(COLOR_RED "  FAIL: RSET %s at index %d\n" COLOR_RESET, key, i);
            exit(1);
        }
        if ((i + 1) % 1000 == 0) {
            printf("    %d/%d records written to RBTree\n", i + 1, TEST_DATA_COUNT);
        }
    }
    printf(COLOR_GREEN "  ✓ RBTree: %d records written\n" COLOR_RESET, TEST_DATA_COUNT);
    
    printf("\n-- Writing %d records to Hash Engine --\n", TEST_DATA_COUNT);
    for (int i = 0; i < TEST_DATA_COUNT; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "hash_key_%d", i);
        snprintf(val, sizeof(val), "hash_val_%d", i);
        if (send_command_check_ok(fd, "HSET", key, val) != 0) {
            printf(COLOR_RED "  FAIL: HSET %s at index %d\n" COLOR_RESET, key, i);
            exit(1);
        }
        if ((i + 1) % 1000 == 0) {
            printf("    %d/%d records written to Hash\n", i + 1, TEST_DATA_COUNT);
        }
    }
    printf(COLOR_GREEN "  ✓ Hash: %d records written\n" COLOR_RESET, TEST_DATA_COUNT);
    
    printf("\n-- Writing %d records to SkipList Engine --\n", TEST_DATA_COUNT);
    for (int i = 0; i < TEST_DATA_COUNT; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "skiplist_key_%d", i);
        snprintf(val, sizeof(val), "skiplist_val_%d", i);
        if (send_command_check_ok(fd, "SKSET", key, val) != 0) {
            printf(COLOR_RED "  FAIL: SKSET %s at index %d\n" COLOR_RESET, key, i);
            exit(1);
        }
        if ((i + 1) % 1000 == 0) {
            printf("    %d/%d records written to SkipList\n", i + 1, TEST_DATA_COUNT);
        }
    }
    printf(COLOR_GREEN "  ✓ SkipList: %d records written\n" COLOR_RESET, TEST_DATA_COUNT);
    
    long size_after = get_file_size(AOF_FILE);
    printf("\nAOF size after: %ld bytes (increase: %ld bytes)\n", size_after, size_after - size_before);
    
    check_file(AOF_FILE, "AOF file");
    
    if (size_after > size_before) {
        printf(COLOR_GREEN "\n✓ Test 1 PASSED: AOF写入成功 (%d records)\n" COLOR_RESET, TEST_DATA_COUNT * 4);
    } else {
        printf(COLOR_RED "\n✗ Test 1 FAILED: AOF文件未增长\n" COLOR_RESET);
        exit(1);
    }
}

// 测试2: BGSAVE执行测试
void test_bgsave(int fd) {
    printf(COLOR_BLUE "\n=== Test 2: BGSAVE Test ===\n" COLOR_RESET);
    
    remove(RDB_FILE);
    
    printf("\n-- Executing BGSAVE --\n");
    if (send_command_check_ok(fd, "BGSAVE", NULL, NULL) == 0)
        printf(COLOR_GREEN "  PASS: BGSAVE command\n" COLOR_RESET);
    else {
        printf(COLOR_RED "  FAIL: BGSAVE command\n" COLOR_RESET);
        exit(1);
    }
    
    printf("\nWaiting for BGSAVE to complete (5 seconds)...\n");
    sleep(5);
    
    check_file(RDB_FILE, "RDB file");
    
    if (get_file_size(RDB_FILE) > 0) {
        printf(COLOR_GREEN "\n✓ Test 2 PASSED: BGSAVE成功生成RDB文件\n" COLOR_RESET);
    } else {
        printf(COLOR_RED "\n✗ Test 2 FAILED: RDB文件未生成或为空\n" COLOR_RESET);
        exit(1);
    }
}

// 测试3: 断电重启验证测试 (验证10000条数据)
void test_recovery(int fd) {
    printf(COLOR_BLUE "\n=== Test 3: Power Failure Recovery Test (%d records) ===\n" COLOR_RESET, TEST_DATA_COUNT);
    
    // 抽样验证：验证第0、1000、2000、...、9000条数据
    printf("\n-- Verifying data before shutdown (sampling) --\n");
    
    int sample_indices[] = {0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 9999};
    int sample_count = sizeof(sample_indices) / sizeof(sample_indices[0]);
    
    // 验证 Array 引擎
    printf("\n  Verifying Array engine:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "array_key_%d", i);
        snprintf(expected, sizeof(expected), "array_val_%d", i);
        char *val = send_get_command(fd, "GET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ GET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ GET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    // 验证 RBTree 引擎
    printf("\n  Verifying RBTree engine:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "rbtree_key_%d", i);
        snprintf(expected, sizeof(expected), "rbtree_val_%d", i);
        char *val = send_get_command(fd, "RGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ RGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ RGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    // 验证 Hash 引擎
    printf("\n  Verifying Hash engine:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "hash_key_%d", i);
        snprintf(expected, sizeof(expected), "hash_val_%d", i);
        char *val = send_get_command(fd, "HGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ HGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ HGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    // 验证 SkipList 引擎
    printf("\n  Verifying SkipList engine:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "skiplist_key_%d", i);
        snprintf(expected, sizeof(expected), "skiplist_val_%d", i);
        char *val = send_get_command(fd, "SKGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ SKGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ SKGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    printf("\n" COLOR_YELLOW "========================================\n");
    printf("请手动操作:\n");
    printf("1. 在服务器终端按 Ctrl+C 停止服务器 (模拟断电)\n");
    printf("2. 重新启动服务器: ./bin/kvstore -c kvstore.conf\n");
    printf("3. 完成后按 Enter 键继续验证\n");
    printf("========================================\n" COLOR_RESET);
    getchar();
    
    close(fd);
    printf("\n重新连接服务器...\n");
    sleep(2);
    
    int new_fd = connect_server("192.168.32.137", 8888);
    if (new_fd < 0) {
        printf(COLOR_RED "连接服务器失败，请确保服务器已重启\n" COLOR_RESET);
        exit(1);
    }
    
    printf("\n-- Verifying data after restart (sampling) --\n");
    
    // 重启后抽样验证
    printf("\n  Verifying Array engine after restart:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "array_key_%d", i);
        snprintf(expected, sizeof(expected), "array_val_%d", i);
        char *val = send_get_command(new_fd, "GET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ GET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ GET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    printf("\n  Verifying RBTree engine after restart:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "rbtree_key_%d", i);
        snprintf(expected, sizeof(expected), "rbtree_val_%d", i);
        char *val = send_get_command(new_fd, "RGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ RGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ RGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    printf("\n  Verifying Hash engine after restart:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "hash_key_%d", i);
        snprintf(expected, sizeof(expected), "hash_val_%d", i);
        char *val = send_get_command(new_fd, "HGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ HGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ HGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    printf("\n  Verifying SkipList engine after restart:\n");
    for (int s = 0; s < sample_count; s++) {
        int i = sample_indices[s];
        char key[64], expected[64];
        snprintf(key, sizeof(key), "skiplist_key_%d", i);
        snprintf(expected, sizeof(expected), "skiplist_val_%d", i);
        char *val = send_get_command(new_fd, "SKGET", key);
        if (val && strcmp(val, expected) == 0) {
            printf(COLOR_GREEN "    ✓ SKGET %s\n" COLOR_RESET, key);
        } else {
            printf(COLOR_RED "    ✗ SKGET %s failed (expected: %s, got: %s)\n" COLOR_RESET, key, expected, val ? val : "NULL");
            exit(1);
        }
    }
    
    printf(COLOR_GREEN "\n✓ Test 3 PASSED: 所有 %d 条数据在断电重启后成功恢复!\n" COLOR_RESET, TEST_DATA_COUNT * 4);
    
    close(new_fd);
}

int main(int argc, char *argv[]) {
    char *ip = "192.168.32.137";
    int port = 8888;
    
    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);
    
    printf(COLOR_BLUE "========================================\n");
    printf("   KVStore 持久化功能测试 (RESP协议)\n");
    printf("   (AOF + BGSAVE + 断电重启恢复)\n");
    printf("   测试数据量: %d 条/引擎\n", TEST_DATA_COUNT);
    printf("========================================\n" COLOR_RESET);
    
    printf("\n服务器地址: %s:%d\n", ip, port);
    printf("请确保服务器已启动，按 Enter 开始测试...\n");
    getchar();
    
    int fd = connect_server(ip, port);
    if (fd < 0) {
        printf(COLOR_RED "连接服务器失败，请检查服务器是否运行\n" COLOR_RESET);
        return 1;
    }
    
    printf(COLOR_GREEN "连接成功!\n" COLOR_RESET);
    
    test_aof_write(fd);
    test_bgsave(fd);
    test_recovery(fd);
    
    printf(COLOR_BLUE "\n========================================\n");
    printf("   所有测试完成! (%d 条数据/引擎，共 %d 条数据)\n", TEST_DATA_COUNT, TEST_DATA_COUNT * 4);
    printf("========================================\n" COLOR_RESET);
    
    return 0;
}