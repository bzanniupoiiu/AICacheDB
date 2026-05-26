#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#define MAX_MSG 4096
#define TOTAL_OPS 1000000   // 100万次操作

// 持久化文件路径
#define RDB_FILE "./Persistence/kvstore.rdb"

// 颜色输出
#define COLOR_RED     "\x1b[31m"
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_YELLOW  "\x1b[33m"
#define COLOR_BLUE    "\x1b[34m"
#define COLOR_RESET   "\x1b[0m"

// 测试配置
typedef struct {
    const char *name;
    int interval;      // BGSAVE 间隔（条数）
    double qps;
    double overhead;
    int success_count;
    double elapsed;
    int bgsave_count;
    double total_bgsave_time;
} test_result_t;

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
    
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// 发送命令并接收响应
int send_cmd_check(int fd, const char *cmd, const char *expected, char *resp_buf, size_t buf_size) {
    ssize_t sent = send(fd, cmd, strlen(cmd), 0);
    if (sent != (ssize_t)strlen(cmd)) return -1;
    
    // 读取响应
    int total = 0;
    while (total < (int)buf_size - 1) {
        ssize_t n = recv(fd, resp_buf + total, 1, 0);
        if (n <= 0) return -1;
        total++;
        if (total >= 2 && resp_buf[total-2] == '\r' && resp_buf[total-1] == '\n') {
            break;
        }
    }
    resp_buf[total] = '\0';
    
    return strncmp(resp_buf, expected, strlen(expected)) == 0 ? 0 : -1;
}

// 执行 BGSAVE
int execute_bgsave(int fd) {
    const char *cmd = "*1\r\n$6\r\nBGSAVE\r\n";
    char resp[64];
    return send_cmd_check(fd, cmd, "+OK", resp, sizeof(resp));
}

// 发送 RSET 命令
int send_rset(int fd, int id) {
    char cmd[256];
    char key[32], val[32];
    snprintf(key, sizeof(key), "key_%d", id);
    snprintf(val, sizeof(val), "val_%d", id);
    snprintf(cmd, sizeof(cmd), "*3\r\n$4\r\nRSET\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
             strlen(key), key, strlen(val), val);
    
    char resp[64];
    return send_cmd_check(fd, cmd, "+OK", resp, sizeof(resp));
}

// 删除 RDB 文件
void clean_rdb_file(void) {
    remove(RDB_FILE);
}

// 获取文件大小（KB）
long get_file_size_kb(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_size / 1024;
    return -1;
}

// 执行单次测试
test_result_t run_test(const char *ip, int port, int interval, const char *name) {
    test_result_t result = {0};
    result.name = name;
    result.interval = interval;
    
    // 清理 RDB 文件
    clean_rdb_file();
    
    int fd = connect_server(ip, port);
    if (fd < 0) {
        fprintf(stderr, "连接服务器失败: %s\n", name);
        return result;
    }
    
    printf("  [%s] 开始测试...\n", name);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    int bgsave_count = 0;
    long total_bgsave_time_us = 0;
    int success = 0;
    
    for (int i = 0; i < TOTAL_OPS; i++) {
        if (send_rset(fd, i) == 0) success++;
        
        // 到达间隔时执行 BGSAVE（最后一条不执行）
        if (interval > 0 && (i + 1) % interval == 0 && i + 1 < TOTAL_OPS) {
            struct timeval bgsave_start, bgsave_end;
            gettimeofday(&bgsave_start, NULL);
            int ret = execute_bgsave(fd);
            gettimeofday(&bgsave_end, NULL);
            
            if (ret == 0) {
                long elapsed_us = (bgsave_end.tv_sec - bgsave_start.tv_sec) * 1000000 +
                                  (bgsave_end.tv_usec - bgsave_start.tv_usec);
                total_bgsave_time_us += elapsed_us;
                bgsave_count++;
            }
            // 短暂等待，避免频繁 fork 导致系统过载
            usleep(100000);
        }
        
        // 进度显示
        if ((i + 1) % 100000 == 0) {
            printf("    已插入 %d 条 (成功: %d)\n", i + 1, success);
        }
    }
    
    gettimeofday(&end, NULL);
    close(fd);
    
    result.elapsed = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
    result.qps = success / result.elapsed;
    result.success_count = success;
    result.bgsave_count = bgsave_count;
    result.total_bgsave_time = total_bgsave_time_us / 1000000.0;
    result.overhead = (result.total_bgsave_time / result.elapsed) * 100;
    
    printf("  [%s] 完成: QPS=%.2f, 耗时=%.2fs, BGSAVE次数=%d, 开销占比=%.2f%%\n\n",
           name, result.qps, result.elapsed, bgsave_count, result.overhead);
    
    return result;
}

// 打印测试结果表格
void print_results(test_result_t *results, int count, double baseline_qps) {
    printf(COLOR_BLUE "\n========================================\n");
    printf("           性能测试结果汇总\n");
    printf("========================================\n" COLOR_RESET);
    
    printf("| %-20s | %-10s | %-12s | %-12s |\n", 
           "BGSAVE 间隔", "插入 QPS", "相对性能", "BGSAVE 开销");
    printf("|----------------------|------------|--------------|--------------|\n");
    
    for (int i = 0; i < count; i++) {
        double ratio = (results[i].qps / baseline_qps) * 100;
        printf("| %-20s | %10.2f | %11.2f%% | %12.2f%% |\n",
               results[i].name, results[i].qps, ratio, results[i].overhead);
    }
    
    printf(COLOR_BLUE "========================================\n" COLOR_RESET);
    
    // 打印 RDB 文件大小
    long rdb_size = get_file_size_kb(RDB_FILE);
    if (rdb_size > 0) {
        printf("\n最终 RDB 文件大小: %ld KB\n", rdb_size);
    }
}

// 完整测试流程
void run_full_test(const char *ip, int port) {
    printf(COLOR_BLUE "\n========================================\n");
    printf("   RDB 持久化性能测试 (BGSAVE 频率影响)\n");
    printf("   总写入量: %d 条 RSET 命令\n", TOTAL_OPS);
    printf("   存储引擎: 红黑树 (RBTree)\n");
    printf("========================================\n" COLOR_RESET);
    
    // 测试配置
    test_result_t results[5];
    int result_count = 0;
    
    // 1. 基准测试（无 BGSAVE）
    printf(COLOR_YELLOW "\n【基准测试】不执行 BGSAVE\n" COLOR_RESET);
    results[result_count] = run_test(ip, port, 0, "无 BGSAVE");
    double baseline_qps = results[result_count].qps;
    result_count++;
    
    sleep(2);
    
    // 2. 每 100 万条执行一次 BGSAVE（即结束时执行一次）
    printf(COLOR_YELLOW "\n【测试 1】每 1,000,000 条执行一次 BGSAVE\n" COLOR_RESET);
    results[result_count] = run_test(ip, port, 1000000, "1,000,000 条/次");
    result_count++;
    sleep(2);
    
    // 3. 每 10 万条执行一次 BGSAVE
    printf(COLOR_YELLOW "\n【测试 2】每 100,000 条执行一次 BGSAVE\n" COLOR_RESET);
    results[result_count] = run_test(ip, port, 100000, "100,000 条/次");
    result_count++;
    sleep(2);
    
    // 4. 每 1 万条执行一次 BGSAVE
    printf(COLOR_YELLOW "\n【测试 3】每 10,000 条执行一次 BGSAVE\n" COLOR_RESET);
    results[result_count] = run_test(ip, port, 10000, "10,000 条/次");
    result_count++;
    sleep(2);
    
    // 5. 每 1 千条执行一次 BGSAVE
    printf(COLOR_YELLOW "\n【测试 4】每 1,000 条执行一次 BGSAVE\n" COLOR_RESET);
    results[result_count] = run_test(ip, port, 1000, "1,000 条/次");
    result_count++;
    
    // 打印汇总结果
    print_results(results, result_count, baseline_qps);
    
    // 结论
    printf(COLOR_GREEN "\n========== 测试结论 ==========\n" COLOR_RESET);
    printf("1. BGSAVE 频率越高，写入 QPS 下降越明显\n");
    printf("2. 当间隔为 1,000 条时，性能下降约 %.1f%%\n", 
           (1 - results[4].qps / baseline_qps) * 100);
    printf("3. BGSAVE 开销占比接近性能下降比例，说明主要瓶颈是快照生成\n");
    printf("4. 建议根据数据安全要求合理配置 save 参数\n");
}

// 快速测试单个模式
void run_single_test(const char *ip, int port, int mode) {
    test_result_t result;
    
    switch(mode) {
        case 1:
            printf(COLOR_YELLOW "\n测试: 每 1,000,000 条执行一次 BGSAVE\n" COLOR_RESET);
            result = run_test(ip, port, 1000000, "1,000,000 条/次");
            break;
        case 2:
            printf(COLOR_YELLOW "\n测试: 每 100,000 条执行一次 BGSAVE\n" COLOR_RESET);
            result = run_test(ip, port, 100000, "100,000 条/次");
            break;
        case 3:
            printf(COLOR_YELLOW "\n测试: 每 10,000 条执行一次 BGSAVE\n" COLOR_RESET);
            result = run_test(ip, port, 10000, "10,000 条/次");
            break;
        case 4:
            printf(COLOR_YELLOW "\n测试: 每 1,000 条执行一次 BGSAVE\n" COLOR_RESET);
            result = run_test(ip, port, 1000, "1,000 条/次");
            break;
        default:
            printf(COLOR_RED "无效的 mode: %d (支持 1-4)\n" COLOR_RESET, mode);
            return;
    }
    
    printf(COLOR_GREEN "\n========== 测试结果 ==========\n" COLOR_RESET);
    printf("插入成功: %d / %d\n", result.success_count, TOTAL_OPS);
    printf("总耗时: %.2f 秒\n", result.elapsed);
    printf("QPS: %.2f\n", result.qps);
    printf("BGSAVE 次数: %d\n", result.bgsave_count);
    printf("BGSAVE 总耗时: %.2f 秒\n", result.total_bgsave_time);
    printf("BGSAVE 开销占比: %.2f%%\n", result.overhead);
}

// 显示帮助信息
void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <ip> <port> <mode>\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "  mode 0: 完整测试 (对比所有 BGSAVE 频率)\n");
    fprintf(stderr, "  mode 1: 每 1,000,000 条执行一次 BGSAVE\n");
    fprintf(stderr, "  mode 2: 每 100,000 条执行一次 BGSAVE\n");
    fprintf(stderr, "  mode 3: 每 10,000 条执行一次 BGSAVE\n");
    fprintf(stderr, "  mode 4: 每 1,000 条执行一次 BGSAVE\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  %s 192.168.32.137 8888 0   # 完整测试\n", prog);
    fprintf(stderr, "  %s 192.168.32.137 8888 1   # 测试 100万条/次\n", prog);
    fprintf(stderr, "  %s 192.168.32.137 8888 2   # 测试 10万条/次\n", prog);
    fprintf(stderr, "  %s 192.168.32.137 8888 3   # 测试 1万条/次\n", prog);
    fprintf(stderr, "  %s 192.168.32.137 8888 4   # 测试 1千条/次\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int mode = atoi(argv[3]);
    
    printf(COLOR_BLUE "========================================\n");
    printf("   KVStore RDB 持久化性能测试工具\n");
    printf("   服务器: %s:%d\n", ip, port);
    printf("   数据量: %d 条 RSET 命令\n", TOTAL_OPS);
    printf("========================================\n" COLOR_RESET);
    
    // 创建 Persistence 目录
    mkdir("./Persistence", 0755);
    
    if (mode == 0) {
        run_full_test(ip, port);
    } else if (mode >= 1 && mode <= 4) {
        run_single_test(ip, port, mode);
    } else {
        printf(COLOR_RED "错误: mode 必须是 0-4 之间的数字\n" COLOR_RESET);
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}