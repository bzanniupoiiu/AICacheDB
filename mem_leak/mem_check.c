#define _GNU_SOURCE
#include "mem_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>

#if MEM_CHECK_ENABLED

static pthread_mutex_t g_mem_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_monitor_thread;
static int g_monitor_running = 0;
static int g_report_interval_sec = 5;  // 默认60秒报告一次

// ---------- 分配/释放记录（文件方式）----------
void *mem_malloc_record(size_t size, const char *file, const char *func, int line) {
    void *ptr = malloc(size);
    if (!ptr) return NULL;

    pthread_mutex_lock(&g_mem_lock);
    mkdir("./mem_block", 0755);  // 确保目录存在

    char path[512];
    snprintf(path, sizeof(path), "./mem_block/%p.mem", ptr);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "[+] %s:%s:%d %p %zu\n", file, func, line, ptr, size);
        fclose(fp);
    }
    pthread_mutex_unlock(&g_mem_lock);
    return ptr;
}

void mem_free_record(void *ptr, const char *file, const char *func, int line) {
    if (!ptr) return;

    pthread_mutex_lock(&g_mem_lock);
    char path[512];
    snprintf(path, sizeof(path), "./mem_block/%p.mem", ptr);
    if (unlink(path) != 0) {
        fprintf(stderr, "[MEM LEAK] Double free or invalid free: %p at %s:%s:%d\n",
                ptr, file, func, line);
    }
    pthread_mutex_unlock(&g_mem_lock);

    free(ptr);
}

// ---------- 生成报告（扫描目录） ----------
void generate_memory_report(void) {
    DIR *dir = opendir("./mem_block");
    if (!dir) {
        // 没有泄漏或目录不存在
        return;
    }

    struct dirent *entry;
    int leak_count = 0;
    printf("\n========== Memory Leak Report ==========\n");
    pthread_mutex_lock(&g_mem_lock);
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[512];
        snprintf(path, sizeof(path), "./mem_block/%s", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (fp) {
            char line[1024];
            if (fgets(line, sizeof(line), fp)) {
                printf("[LEAK] %s", line);
                leak_count++;
            }
            fclose(fp);
        }
    }
    pthread_mutex_unlock(&g_mem_lock);
    closedir(dir);
    printf("========================================\n");
    printf("Total unfreed blocks: %d\n", leak_count);
    fflush(stdout);
}

// ---------- 监控线程：定期生成报告 ----------
static void *monitor_thread_func(void *arg) {
    while (g_monitor_running) {
        sleep(g_report_interval_sec);
        if (g_monitor_running) {
            generate_memory_report();
        }
    }
    return NULL;
}

// ---------- 对外接口 ----------
void mem_tracker_init(void) {
    mkdir("./mem_block", 0755);
    g_monitor_running = 1;
    if (pthread_create(&g_monitor_thread, NULL, monitor_thread_func, NULL) != 0) {
        perror("pthread_create for mem tracker");
        g_monitor_running = 0;
    } else {
        printf("Memory leak tracker started (report every %d seconds)\n", g_report_interval_sec);
    }
}

void mem_tracker_stop(void) {
    if (!g_monitor_running) return;
    g_monitor_running = 0;
    pthread_join(g_monitor_thread, NULL);
    // 最后再生成一次最终报告
    generate_memory_report();
}

#endif // MEM_CHECK_ENABLED