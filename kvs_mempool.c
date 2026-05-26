#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//小块内存节点
typedef struct mp_node_s
{
    /* data */
    struct mp_node_s *next;
    unsigned char *last;
    unsigned char *end;
} mp_node_t;

//大块内存节点
typedef struct mp_large_s
{
    /* data */
    struct mp_large_s *next;
    void *alloc;//实际分配给用户的内存地址
}mp_large_t;
//内存池控制结构

typedef struct mp_pool_s
{
    /* data */
    size_t max;
    struct mp_node_s* head;
    struct mp_large_s* large;
}mp_pool_t;


/*
创建内存池
block_size建议4096
return 0成功，-1失败
*/
int kvs_mempool_create(mp_pool_t *m,int block_size)
{
    if(block_size<=(int)sizeof(mp_node_t))
    {
        return -1;
    }
    mp_node_t *node = (mp_node_t *) malloc(block_size);//头节点
    if(!node)
    {
        return -1;
    }
    //初始化第一个节点
    node->last = (unsigned char *)node+ sizeof(mp_node_t);
    node->end = (unsigned char *)node+block_size;
    node->next = NULL;

    //初始化内存池
    m->head = node;
    m->large = NULL;
    m->max = block_size-sizeof(mp_node_t);//阈值

    return 0;


}


void kvs_mempool_destroy(mp_pool_t *m)
{
    //1、先释放大块存储的节点
    mp_large_t *large = m->large;
    while (large!=NULL)
    {
        /* code */
        mp_large_t *next = large->next;
        //大块内存的实际分配地址在alloc前面的 sizeof(size_t)的地方
        void *real_ptr = (char *)large->alloc - sizeof(size_t);
        free(real_ptr);
        free(large);
        large = next;
    }
    m->large = NULL;

    //2、再释放小块内存的节点
    mp_node_t *node = m->head;
    while(node)
    {
        mp_node_t *next = node->next;
        free(node);
        node = next;
    }
    m->head = NULL;
}


/*
从内存池中分配内存
*/

void *kvs_mempool_alloc(mp_pool_t *m,size_t size)
{
    if(size == 0) return NULL;

    if(size<=m->max)//可以从小块内存中分配
    {
        mp_node_t *node = m->head;
        mp_node_t *prev = NULL;

        while (node)
        {
            /* code */
            size_t remain_space_r = node->end - node->last;
            if(remain_space_r>=size)
            {
                void *ptr = node->last;
                node->last +=size;
                return ptr;
            }
            prev = node;
            node = node->next;//第二个节点node，一个node对应一个1024（比如说）
        }
        // 没有可用节点，开辟新节点
        size_t node_size = m->max + sizeof(mp_node_t); // 节点总大小
        mp_node_t *new_node = (mp_node_t *)malloc(node_size);
        if (!new_node) return NULL;

        new_node->last = (unsigned char *)new_node + sizeof(mp_node_t);
        new_node->end = (unsigned char *)new_node + node_size;
        new_node->next = NULL;

        // 将新节点链接到链表尾部
        if (prev) {
            prev->next = new_node;
        } else {
            m->head = new_node; 
        }

        // 从新节点分配
        void *ptr = new_node->last;
        new_node->last += size;
        return ptr;

    }else{
        // 大块内存分配
        size_t alloc_size = size + sizeof(size_t);
        void *raw_ptr = malloc(alloc_size);
        if (!raw_ptr) return NULL;

        // 存储大小信息
        *(size_t *)raw_ptr = size;
        void *user_ptr = (char *)raw_ptr + sizeof(size_t);

        // 创建大块节点并插入链表头部
        mp_large_t *large_node = (mp_large_t *)malloc(sizeof(mp_large_t));
        if (!large_node) {
            free(raw_ptr);
            return NULL;
        }
        large_node->alloc = user_ptr;
        large_node->next = m->large;
        m->large = large_node;

        return user_ptr;
    }

}


void  kvs_mempool_free(mp_pool_t *m, void *ptr)
{
    mp_large_t **plarge = &(m->large);
    while (*plarge) {
        if ((*plarge)->alloc == ptr) {
            mp_large_t *to_free = *plarge;
            *plarge = to_free->next; // 从链表中移除

            // 获取实际分配地址并释放
            void *real_ptr = (char *)ptr - sizeof(size_t);
            free(real_ptr);
            free(to_free);
            return;
        }
        plarge = &((*plarge)->next);
    }
    // 如果是小块内存，不做任何操作,整个池销毁时统一释放

}

#if 0
// 简单的测试示例
int main() {
    mp_pool_t pool;
    if (kvs_mempool_create(&pool, 4096) != 0) {
        printf("create pool failed\n");
        return -1;
    }

    // 分配小块内存（小于等于 max = 4096 - sizeof(mp_node_t) ≈ 4088）
    void *p1 = kvs_mempool_alloc(&pool, 100);
    void *p2 = kvs_mempool_alloc(&pool, 200);
    printf("p1 = %p, p2 = %p\n", p1, p2);

    // 分配大块内存
    void *p3 = kvs_mempool_alloc(&pool, 5000);
    printf("p3 = %p\n", p3);

    // 释放大块内存
    kvs_mempool_free(&pool, p3);

    // 再次分配大块（验证释放后可用）
    void *p4 = kvs_mempool_alloc(&pool, 3000);
    printf("p4 = %p\n", p4);
    kvs_mempool_free(&pool, p4);

    // 销毁内存池
    kvs_mempool_destroy(&pool);
    return 0;
}
#endif
