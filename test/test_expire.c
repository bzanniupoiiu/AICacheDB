// test_mass_expire.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 8888
#define HOST "127.0.0.1"
#define PER_ENGINE 2500
#define WAIT_SEC 20

int send_cmd(int fd, const char *cmd, char *resp, size_t sz) {
    send(fd, cmd, strlen(cmd), 0);
    int n = recv(fd, resp, sz-1, 0);
    if (n > 0) resp[n] = '\0';
    return n;
}

int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {AF_INET, htons(PORT), inet_addr(HOST)};
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    const char *engines[][2] = {
        {"SETEX", "GET"},
        {"RSETEX", "RGET"},
        {"HSETEX", "HGET"},
        {"SKSETEX", "SKGET"}
    };
    const char *names[] = {"array", "rbtree", "hash", "skiplist"};

    // 插入数据
    printf("插入 %d 条数据（过期时间10秒）...\n", PER_ENGINE * 4);
    for (int e = 0; e < 4; e++) {
        const char *setex = engines[e][0];
        for (int i = 0; i < PER_ENGINE; i++) {
            char key[32], val[32];
            snprintf(key, sizeof(key), "%s_%d", names[e], i);
            snprintf(val, sizeof(val), "v%d", i);
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "*4\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n$2\r\n10\r\n$%zu\r\n%s\r\n",
                     strlen(setex), setex, strlen(key), key, strlen(val), val);
            char resp[32];
            send_cmd(fd, cmd, resp, sizeof(resp));
            if (i % 500 == 0) printf("%s: %d/%d\n", names[e], i, PER_ENGINE);
        }
        printf("%s 插入完成\n", names[e]);
    }

    printf("等待 %d 秒...\n", WAIT_SEC);
    sleep(WAIT_SEC);

    // 检查是否存在
    int found = 0;
    for (int e = 0; e < 4; e++) {
        const char *get = engines[e][1];
        for (int i = 0; i < PER_ENGINE; i++) {
            char key[32];
            snprintf(key, sizeof(key), "%s_%d", names[e], i);
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "*2\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                     strlen(get), get, strlen(key), key);
            char resp[64];
            if (send_cmd(fd, cmd, resp, sizeof(resp)) > 0 && strncmp(resp, "$-1", 3) != 0) {
                found++;
            }
        }
        printf("%s 剩余键数: %d\n", names[e], found - e * PER_ENGINE);
    }

    printf("总剩余键数: %d (期望 0)\n", found);
    close(fd);
    return found == 0 ? 0 : 1;
}