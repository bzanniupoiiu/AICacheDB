#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>

#define TOTAL_OPS 1000000          // 总操作次数
#define PROGRESS_STEP 100000       // 进度打印间隔
#define BUFFER_SIZE 1024           // 读写缓冲区大小
#define RESPONSE_TIMEOUT_SEC 2     // 响应超时(秒)

#define VALUE_SIZE (100 * 1024)   // 每个 value 100KB

static int connect_server(const char *ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }

    // 禁用Nagle算法，降低延迟
    // int flag = 1;
    // setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    return sock;
}

// 发送完整的RESP命令，并读取一行响应（以\n结尾）
// 返回0成功，-1失败
static int send_command_and_check(int sock, const char *resp_cmd, const char *expected) {
    if (send(sock, resp_cmd, strlen(resp_cmd), 0) != (ssize_t)strlen(resp_cmd)) {
        perror("send");
        return -1;
    }

    char buf[BUFFER_SIZE];
    int total = 0;
    // 读取直到遇到换行符或缓冲区满
    while (total < BUFFER_SIZE - 1) {
        int n = recv(sock, buf + total, 1, 0);
        if (n <= 0) {
            if (n == 0) fprintf(stderr, "连接关闭\n");
            else perror("recv");
            return -1;
        }
        total += n;
        if (buf[total-1] == '\n') break;
    }
    buf[total] = '\0';

    // 检查响应是否以预期字符串开头（例如 "+OK\r\n" 检查 "+OK"）
    if (strncmp(buf, expected, strlen(expected)) != 0) {
        fprintf(stderr, "Unexpected response: %s (expected %s)\n", buf, expected);
        return -1;
    }
    return 0;
}

// 构造RESP数组命令，返回动态分配的字符串，调用者需free
static char* build_resp_array(int argc, const char **argv) {
    int total_len = 0;
    // 计算 *<argc>\r\n
    char header[32];
    int header_len = snprintf(header, sizeof(header), "*%d\r\n", argc);
    total_len += header_len;

    // 为每个参数计算 $len\r\narg\r\n
    int *arg_lens = malloc(argc * sizeof(int));
    for (int i = 0; i < argc; i++) {
        arg_lens[i] = strlen(argv[i]);
        total_len += snprintf(NULL, 0, "$%d\r\n", arg_lens[i]) + arg_lens[i] + 2; // +2 for \r\n after arg
    }

    char *cmd = malloc(total_len + 1);
    if (!cmd) {
        free(arg_lens);
        return NULL;
    }
    char *ptr = cmd;
    memcpy(ptr, header, header_len);
    ptr += header_len;

    for (int i = 0; i < argc; i++) {
        ptr += sprintf(ptr, "$%d\r\n", arg_lens[i]);
        memcpy(ptr, argv[i], arg_lens[i]);
        ptr += arg_lens[i];
        *ptr++ = '\r';
        *ptr++ = '\n';
    }
    *ptr = '\0';
    free(arg_lens);
    return cmd;
}

// 模式0/2/4: 插入测试 (mode_name: "RSET" 或 "SET")
static void test_insert(const char *ip, int port, const char *cmd_name, const char *test_title) {
    printf("\n========== %s ==========\n", test_title);
    printf("开始插入 %d 条数据...\n", TOTAL_OPS);

    int sock = connect_server(ip, port);
    if (sock < 0) exit(1);

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 1; i <= TOTAL_OPS; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key:%d", i);
        snprintf(val, sizeof(val), "val:%d", i);
        const char *argv[] = {cmd_name, key, val};
        char *resp = build_resp_array(3, argv);
        if (!resp) {
            fprintf(stderr, "build_resp_array失败\n");
            close(sock);
            exit(1);
        }
        if (send_command_and_check(sock, resp, "+OK") != 0) {
            free(resp);
            close(sock);
            exit(1);
        }
        free(resp);

        if (i % PROGRESS_STEP == 0) {
            printf("已插入 %d 条数据\n", i);
        }
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double qps = TOTAL_OPS / elapsed;
    printf("插入完成，耗时 %.2f 秒，QPS = %.2f\n", elapsed, qps);
    close(sock);
}

static void test_insert_large(const char *ip, int port) {
    printf("开始插入 %d 条数据，每条 value %d KB，总数据量约 %.1f GB\n", 
           TOTAL_OPS, VALUE_SIZE/1024, (double)TOTAL_OPS * VALUE_SIZE / (1024*1024*1024));
    
    int sock = connect_server(ip, port);
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    for (int i = 1; i <= TOTAL_OPS; i++) {
        char key[32];
        char *value = malloc(VALUE_SIZE);
        if (!value) exit(1);
        
        snprintf(key, sizeof(key), "key:%d", i);
        memset(value, 'A' + (i % 26), VALUE_SIZE - 1);
        value[VALUE_SIZE - 1] = '\0';
        
        const char *argv[] = {"SET", key, value};
        char *resp = build_resp_array(3, argv);
        
        if (send_command_and_check(sock, resp, "+OK") != 0) {
            free(value);
            free(resp);
            break;
        }
        
        free(value);
        free(resp);
        
        if (i % PROGRESS_STEP == 0) {
            printf("已插入 %d 条数据 (%.1f GB)\n", i, 
                   (double)i * VALUE_SIZE / (1024*1024*1024));
        }
    }
    
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("插入完成，耗时 %.2f 秒，平均速度 %.2f MB/s\n", 
           elapsed, (double)TOTAL_OPS * VALUE_SIZE / elapsed / (1024*1024));
    close(sock);
}
// 连接服务器，返回socket fd
// 模式1/5: PING测试
static void test_ping(const char *ip, int port, const char *test_title) {
    printf("\n========== %s ==========\n", test_title);
    printf("开始发送 %d 次 PING...\n", TOTAL_OPS);

    int sock = connect_server(ip, port);
    if (sock < 0) exit(1);

    const char *argv[] = {"PING"};
    char *resp_cmd = build_resp_array(1, argv);
    if (!resp_cmd) {
        close(sock);
        exit(1);
    }

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 1; i <= TOTAL_OPS; i++) {
        if (send_command_and_check(sock, resp_cmd, "+PONG") != 0) {
            free(resp_cmd);
            close(sock);
            exit(1);
        }
        if (i % PROGRESS_STEP == 0) {
            printf("已完成 %d 次 PING\n", i);
        }
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double qps = TOTAL_OPS / elapsed;
    printf("PING 完成，耗时 %.2f 秒，QPS = %.2f\n", elapsed, qps);
    free(resp_cmd);
    close(sock);
}

// 模式3: BGSAVE测试，每interval条数据后执行一次BGSAVE（只发送，不检查响应）
static void test_bgsave(const char *ip, int port, int interval) {
    printf("\n========== RDB BGSAVE 性能测试 (每 %d 条 BGSAVE，fire-and-forget) ==========\n", interval);
    printf("开始插入 %d 条数据，每插入 %d 条执行一次 BGSAVE...\n", TOTAL_OPS, interval);

    int sock = connect_server(ip, port);
    if (sock < 0) exit(1);
    int bgsave_count = TOTAL_OPS / interval;   // BGSAVE 执行次数
    int total_ops = TOTAL_OPS + bgsave_count;  // 总命令数

    struct timeval start, end;
    gettimeofday(&start, NULL);

    for (int i = 1; i <= TOTAL_OPS; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key:%d", i);
        snprintf(val, sizeof(val), "val:%d", i);
        const char *argv[] = {"RSET", key, val};
        char *resp = build_resp_array(3, argv);
        if (!resp) {
            close(sock);
            exit(1);
        }
        // 只发送，不等待响应，忽略返回值
        send(sock, resp, strlen(resp), 0);
        free(resp);

        // 每插入 interval 条后执行 BGSAVE
        if (i % interval == 0) {
            const char *bgsave_argv[] = {"BGSAVE"};
            char *bgsave_cmd = build_resp_array(1, bgsave_argv);
            if (!bgsave_cmd) {
                close(sock);
                exit(1);
            }
            send(sock, bgsave_cmd, strlen(bgsave_cmd), 0);
            free(bgsave_cmd);
        }

        if (i % PROGRESS_STEP == 0) {
            printf("已插入 %d 条数据\n", i);
        }
    }

    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    double qps_total = total_ops / elapsed;    // 整体 QPS（含 BGSAVE）
    double qps_set = TOTAL_OPS / elapsed;      // SET 的 QPS（仅供参考）

    printf("插入完成，总操作数 %d (SET: %d, BGSAVE: %d)，耗时 %.2f 秒\n",
           total_ops, TOTAL_OPS, bgsave_count, elapsed);
    printf("整体 QPS (SET+BGSAVE): %.2f, SET QPS (含BGSAVE耗时): %.2f\n",
           qps_total, qps_set);
    close(sock);
}
static void print_banner() {
    printf("========================================\n");
    printf("   KVStore 持久化性能测试\n");
    printf("   数据量: %d 条\n", TOTAL_OPS);
    printf("========================================\n");
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "用法: %s <ip> <port> <mode> [bgsave_interval]\n", argv[0]);
        fprintf(stderr, "mode: 0=RSET插入(AOF), 1=PING, 2=预留(同0), 3=BGSAVE测试, 4=SET插入, 5=PING(Redis风格)\n");
        fprintf(stderr, "示例: %s 192.168.32.136 8888 0\n", argv[0]);
        fprintf(stderr, "      %s 192.168.32.136 8888 3 10000\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);

    print_banner();

    switch (mode) {
        case 0:
            test_insert(ip, port, "RSET", "AOF 性能测试");
            break;
        case 1:
            test_ping(ip, port, "PING 性能测试");
            break;
        case 2:
            test_insert_large(ip, port);
            break;
        case 3:
            {
                if (argc < 5) {
                    fprintf(stderr, "模式3需要指定bgsave间隔(例如1000,10000,100000,1000000)\n");
                    return 1;
                }
                int interval = atoi(argv[4]);
                if (interval <= 0 || interval > TOTAL_OPS) {
                    fprintf(stderr, "无效的间隔: %d (应在1~%d之间)\n", interval, TOTAL_OPS);
                    return 1;
                }
                test_bgsave(ip, port, interval);
            }
            break;
        case 4:
            test_insert(ip, port, "SET", "Redis SET 性能测试");
            break;
        case 5:
            test_ping(ip, port, "Redis PING 性能测试");
            break;
        default:
            fprintf(stderr, "未知模式: %d\n", mode);
            return 1;
    }

    return 0;
}