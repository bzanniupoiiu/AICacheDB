#ifndef MEM_CHECK_H
#define MEM_CHECK_H

#include <stddef.h>

// 启用内存检测（1=启用，0=禁用）
#define MEM_CHECK_ENABLED 1

#if MEM_CHECK_ENABLED

// 分配/释放记录函数
void *mem_malloc_record(size_t size, const char *file, const char *func, int line);
void mem_free_record(void *ptr, const char *file, const char *func, int line);

// 替换 kvs_malloc / kvs_free
#undef kvs_malloc
#undef kvs_free
#define kvs_malloc(size) mem_malloc_record(size, __FILE__, __func__, __LINE__)
#define kvs_free(ptr)    mem_free_record(ptr, __FILE__, __func__, __LINE__)

// 初始化跟踪系统（创建目录、启动监控线程）
void mem_tracker_init(void);

// 停止监控线程（在程序退出前调用）
void mem_tracker_stop(void);

// 生成一次性报告（也可手动调用）
void generate_memory_report(void);

#else
// 禁用检测时，直接使用原生 malloc/free
#define kvs_malloc(size) malloc(size)
#define kvs_free(ptr)    free(ptr)
#endif

#endif