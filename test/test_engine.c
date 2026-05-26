// test_all_engines_refactored.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8888
#define HOST "127.0.0.1"
#define COUNT 10000          // 每个引擎数据量
#define MOD_COUNT 100        // 修改的数量
#define DEL_COUNT 100        // 删除的数量

// 引擎配置
typedef struct {
    const char *set_cmd;   // 设置命令
    const char *get_cmd;   // 获取命令
    const char *del_cmd;   // 删除命令
    const char *mod_cmd;   // 修改命令
    const char *exist_cmd; // 存在命令
    const char *expire_cmd;// 过期命令
    const char *ttl_cmd;   // TTL命令
    const char *setex_cmd; // SETEX命令
    const char *name;      // 引擎名称
} Engine;

Engine engines[] = {
    {"SET", "GET", "DEL", "MOD", "EXIST", "EXPIRE", "TTL", "SETEX", "array"},
    {"RSET", "RGET", "RDEL", "RMOD", "REXIST", "REXPIRE", "RTTL", "RSETEX", "rbtree"},
    {"HSET", "HGET", "HDEL", "HMOD", "HEXIST", "HEXPIRE", "HTTL", "HSETEX", "hash"},
    {"SKSET", "SKGET", "SKDEL", "SKMOD", "SKEXIST", "SKEXPIRE", "SKTTL", "SKSETEX", "skiplist"}
};
const int engine_cnt = 4;

// 读取指定字节数（确保全部读取）
int recv_full(int fd, char *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) return -1;
        received += n;
    }
    return 0;
}

// 读取一行（以 \r\n 结尾），line 缓冲区至少 size 字节，返回读取的字节数（不含结尾 \0），失败返回 -1
int read_line(int fd, char *line, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c;
        if (recv(fd, &c, 1, 0) != 1) return -1;
        if (c == '\n') {
            if (i > 0 && line[i-1] == '\r') {
                line[i-1] = '\0';   // 去掉 \r\n
                return i - 1;
            } else {
                line[i] = '\0';
                return i;
            }
        }
        line[i++] = c;
    }
    return -1; // 缓冲区不足
}

// 发送 RESP 命令并读取完整响应
// 返回值: 0 成功，-1 失败
// 对于简单字符串（+===================），resp 存储去掉 "+" 后的内容（不含 \r\n）
// 对于整数（:===================），resp 存储整数字符串
// 对于批量字符串（$===================），resp 存储数据内容（不含长度和 \r\n）
// 对于错误（-===================），resp 存储错误信息
int send_command(int fd, const char *cmd, char *resp, size_t resp_sz) {
    // 发送命令
    if (send(fd, cmd, strlen(cmd), 0) != (ssize_t)strlen(cmd))
        return -1;

    // 读取响应第一行
    char header[128];
    if (read_line(fd, header, sizeof(header)) < 0)
        return -1;

    char type = header[0];
    if (type == '+') { // 简单字符串
        strncpy(resp, header + 1, resp_sz - 1);
        resp[resp_sz - 1] = '\0';
        return 0;
    } else if (type == ':') { // 整数
        strncpy(resp, header + 1, resp_sz - 1);
        resp[resp_sz - 1] = '\0';
        return 0;
    } else if (type == '-') { // 错误
        strncpy(resp, header + 1, resp_sz - 1);
        resp[resp_sz - 1] = '\0';
        return -1; // 返回错误，但 resp 包含错误信息
    } else if (type == '$') { // 批量字符串
        int len = atoi(header + 1);
        if (len == -1) { // nil
            resp[0] = '\0';
            return 0;
        }
        if ((size_t)len >= resp_sz) {
            fprintf(stderr, "Buffer too small for bulk string of length %d\n", len);
            return -1;
        }
        // 读取数据 + \r\n
        if (recv_full(fd, resp, len + 2) < 0)
            return -1;
        resp[len] = '\0'; // 去掉尾部的 \r\n
        return 0;
    } else {
        fprintf(stderr, "Unknown RESP type: %c\n", type);
        return -1;
    }
}

// 构造 RESP 命令（通用）
void build_cmd(char *buf, size_t sz, const char *cmd, const char *key, const char *val) {
    if (val == NULL) {
        if (key == NULL) {
            snprintf(buf, sz, "*1\r\n$%zu\r\n%s\r\n", strlen(cmd), cmd);
        } else {
            snprintf(buf, sz, "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(cmd), cmd, strlen(key), key);
        }
    } else {
        snprintf(buf, sz, "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                 strlen(cmd), cmd, strlen(key), key, strlen(val), val);
    }
}

void build_cmd_int(char *buf, size_t sz, const char *cmd, const char *key, int seconds) {
    char sec_str[16];
    snprintf(sec_str, sizeof(sec_str), "%d", seconds);
    snprintf(buf, sz, "*3\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(cmd), cmd, strlen(key), key, strlen(sec_str), sec_str);
}

void build_cmd_setex(char *buf, size_t sz, const char *cmd, const char *key, int seconds, const char *val) {
   char sec_str[16];
    snprintf(sec_str, sizeof(sec_str), "%d", seconds);
    snprintf(buf, sz, "*4\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
            strlen(cmd), cmd, strlen(key), key, strlen(val), val, strlen(sec_str), sec_str);
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {AF_INET, htons(PORT), inet_addr(HOST)};
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    char resp[1024]; // 足够容纳数据
    int ok = 1;

    for (int e = 0; e < engine_cnt; e++) {
        const Engine *eng = &engines[e];
        printf("\n========== Testing %s engine ==========\n", eng->name);

        // 1. SET 10000 条数据
        printf("SET %d keys===================\n", COUNT);
        for (int i = 0; i < COUNT; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            snprintf(val, sizeof(val), "val_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->set_cmd, key, val);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "OK") != 0) {
                printf("  SET %s failed: %s\n", key, resp);
                ok = 0; goto out;
            }
            if ((i+1) % 1000 == 0) printf("    %d done\n", i+1);
        }

        // 2. GET 所有键
        printf("GET all keys===================\n");
        for (int i = 0; i < COUNT; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            snprintf(expected, sizeof(expected), "val_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->get_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0) {
                printf("  GET %s failed\n", key);
                ok = 0; goto out;
            }
            if (resp[0] == '\0') { // nil
                printf("  GET %s returned nil\n", key);
                ok = 0; goto out;
            }
            if (strcmp(resp, expected) != 0) {
                printf("  GET %s value mismatch: %s vs %s\n", key, resp, expected);
                ok = 0; goto out;
            }
            if ((i+1) % 1000 == 0) printf("    %d done\n", i+1);
        }

        // 3. MOD 前100个键
        printf("MOD first %d keys===================\n", MOD_COUNT);
        for (int i = 0; i < MOD_COUNT; i++) {
            char key[32], new_val[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            snprintf(new_val, sizeof(new_val), "new_val_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->mod_cmd, key, new_val);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "OK") != 0) {
                printf("  MOD %s failed\n", key);
                ok = 0; goto out;
            }
        }

        // 4. 验证MOD后的值
        printf("Verify MOD===================\n");
        for (int i = 0; i < MOD_COUNT; i++) {
            char key[32], expected[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            snprintf(expected, sizeof(expected), "new_val_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->get_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0) {
                printf("  GET %s failed\n", key);
                ok = 0; goto out;
            }
            if (strcmp(resp, expected) != 0) {
                printf("  GET %s after MOD mismatch\n", key);
                ok = 0; goto out;
            }
        }

        // 5. EXIST 检查前100个（存在）和后100个（不存在）
        printf("EXIST test===================\n");
        for (int i = 0; i < MOD_COUNT; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->exist_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "1") != 0) {
                printf("  EXIST %s returned %s (expected 1)\n", key, resp);
                ok = 0; goto out;
            }
        }
        for (int i = COUNT; i < COUNT + 100; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->exist_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "0") != 0) {
                printf("  EXIST %s returned %s (expected 0)\n", key, resp);
                ok = 0; goto out;
            }
        }

        // 6. DEL 后100个键
        printf("DEL last %d keys===================\n", DEL_COUNT);
        for (int i = COUNT - DEL_COUNT; i < COUNT; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->del_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "1") != 0) {
                printf("  DEL %s failed\n", key);
                ok = 0; goto out;
            }
        }

        // 7. EXPIRE 设置中间100个键过期（5秒）
        printf("EXPIRE middle 100 keys (5s)===================\n");
        for (int i = COUNT/2; i < COUNT/2 + 100; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd_int(cmd, sizeof(cmd), eng->expire_cmd, key, 5);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "1") != 0) {
                printf("  EXPIRE %s failed\n", key);
                ok = 0; goto out;
            }
        }

        // 8. TTL 检查这些键的TTL（应在1-5秒之间）
        printf("TTL check===================\n");
        sleep(2); // 等待2秒
        for (int i = COUNT/2; i < COUNT/2 + 100; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key_%s_%d", eng->name, i);
            char cmd[512];
            build_cmd(cmd, sizeof(cmd), eng->ttl_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0) {
                printf("  TTL %s failed\n", key);
                ok = 0; goto out;
            }
            int ttl = atoi(resp);
            if (ttl <= 0 || ttl > 3) { // 2秒后剩余应在1-3秒之间
                printf("  TTL %s = %d (expected 1-3)\n", key, ttl);
                ok = 0; goto out;
            }
        }

        // 9. SETEX 测试：设置一个新键并立即检查，5秒后再次检查应过期
        printf("SETEX test===================\n");
        {
            char key[32], val[32];
            snprintf(key, sizeof(key), "setex_%s", eng->name);
            snprintf(val, sizeof(val), "temp_val");
            char cmd[512];
            build_cmd_setex(cmd, sizeof(cmd), eng->setex_cmd, key, 5, val);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0 || strcmp(resp, "OK") != 0) {
                printf("  SETEX failed\n");
                ok = 0; goto out;
            }
            // 立即GET应存在
            build_cmd(cmd, sizeof(cmd), eng->get_cmd, key, NULL);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0) {
                printf("  GET after SETEX failed\n");
                ok = 0; goto out;
            }
            if (resp[0] == '\0') {
                printf("  GET after SETEX returned nil\n");
                ok = 0; goto out;
            }
            // 等待6秒后应过期
            sleep(6);
            if (send_command(fd, cmd, resp, sizeof(resp)) != 0) {
                printf("  GET after sleep failed\n");
                ok = 0; goto out;
            }
            if (resp[0] != '\0') {
                printf("  GET after sleep returned value (should be nil)\n");
                ok = 0; goto out;
            }
        }

        printf("%s engine all tests passed!\n", eng->name);
    }

out:
    close(fd);
    return ok ? 0 : 1;
}
