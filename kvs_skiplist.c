#include "kvstore.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

kvs_skiplist_t global_skiplist;

// 随机层数生成，参考示例代码，概率 0.5，层数范围 [0, SKIPLIST_MAX_LEVEL]
static int random_level(void) {
    int level = 0;
    while (rand() < RAND_MAX / 2 && level < SKIPLIST_MAX_LEVEL) {
        level++;
    }
    return level;
}

// 创建一个新节点（键和值会被拷贝存储）
static skiplist_node_t* skiplist_new_node(int level, const char *key, const char *value, long long expire_ms) {
    skiplist_node_t *node = (skiplist_node_t*)kvs_malloc(sizeof(skiplist_node_t));
    if (!node) return NULL;

    if (key) {
        node->key = kvs_malloc(strlen(key) + 1);
        if (!node->key) { kvs_free(node); return NULL; }
        strcpy(node->key, key);
    } else {
        node->key = NULL;
    }

    if (value) {
        node->value = kvs_malloc(strlen(value) + 1);
        if (!node->value) {
            if (node->key) kvs_free(node->key);
            kvs_free(node);
            return NULL;
        }
        strcpy(node->value, value);
    } else {
        node->value = NULL;
    }

    node->forward = (skiplist_node_t**)kvs_malloc((level + 1) * sizeof(skiplist_node_t*));
    if (!node->forward) {
        if (node->key) kvs_free(node->key);
        if (node->value) kvs_free(node->value);
        kvs_free(node);
        return NULL;
    }
    for (int i = 0; i <= level; i++) {
        node->forward[i] = NULL;
    }
    node->expire_ms = expire_ms;   // 设置过期时间
    return node;
}

// 释放节点内存（包括键和值）
static void skiplist_free_node(skiplist_node_t *node) {
    if (node->key) kvs_free(node->key);
    if (node->value) kvs_free(node->value);
    kvs_free(node->forward);
    kvs_free(node);
}

// 创建跳表实例
int kvs_skiplist_create(kvs_skiplist_t *inst) {
    if (!inst) return -1;

    // 初始化随机种子（只做一次）
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }

    inst->level = 0;
    inst->size = 0;
    // 创建头节点，层数为 MAX_LEVEL（头节点不存储数据，key/value 为 NULL）
    inst->header = skiplist_new_node(SKIPLIST_MAX_LEVEL, NULL, NULL, -1);
    if (!inst->header) return -1;

    return 0;
}

// 销毁跳表，释放所有节点
void kvs_skiplist_destroy(kvs_skiplist_t *inst) {
    if (!inst) return;

    // 从最底层开始遍历释放所有数据节点
    skiplist_node_t *current = inst->header->forward[0];
    while (current) {
        skiplist_node_t *tmp = current;
        current = current->forward[0];
        skiplist_free_node(tmp);
    }
    // 释放头节点
    skiplist_free_node(inst->header);
    inst->header = NULL;
    inst->level = 0;
    inst->size = 0;
}

// 辅助函数：查找指定键的前驱节点数组（用于插入和删除）
static void skiplist_find_predecessors(kvs_skiplist_t *inst, const char *key, skiplist_node_t **update) {
    skiplist_node_t *current = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
        update[i] = current;
    }
}

// 设置键值对（如果键已存在则返回 1，插入成功返回 0）
int kvs_skiplist_set(kvs_skiplist_t *inst, char *key, char *value, long long expire_ms) {
    if (!inst || !key || !value) return -1;

    skiplist_node_t *update[SKIPLIST_MAX_LEVEL + 1];
    skiplist_find_predecessors(inst, key, update);
    skiplist_node_t *target = update[0]->forward[0];

    if (target && strcmp(target->key, key) == 0) {
        // 已存在，更新值
        kvs_free(target->value);
        target->value = kvs_malloc(strlen(value) + 1);
        if (!target->value) return -2;
        strcpy(target->value, value);
        target->expire_ms = expire_ms;
        return 0;
    }

    int level = random_level();
    if (level > inst->level) {
        for (int i = inst->level + 1; i <= level; i++) {
            update[i] = inst->header;
        }
        inst->level = level;
    }

    skiplist_node_t *new_node = skiplist_new_node(level, key, value, expire_ms);
    if (!new_node) return -2;

    for (int i = 0; i <= level; i++) {
        new_node->forward[i] = update[i]->forward[i];
        update[i]->forward[i] = new_node;
    }
    inst->size++;
    return 0;
}



// 获取键对应的值，若不存在返回 NULL
char* kvs_skiplist_get(kvs_skiplist_t *inst, char *key) {
    if (!inst || !key) return NULL;

    skiplist_node_t *current = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    if (current && strcmp(current->key, key) == 0) {
        long long now = kvs_current_time_ms();
        if (current->expire_ms != -1 && current->expire_ms <= now) {
            kvs_skiplist_del(inst, key);
            return NULL;
        }
        return current->value;
    }
    return NULL;
}

// 删除键值对，成功返回 0，不存在返回 1，错误返回负数
int kvs_skiplist_del(kvs_skiplist_t *inst, char *key) {
    if (!inst || !key) return -1;

    skiplist_node_t *update[SKIPLIST_MAX_LEVEL + 1];
    skiplist_find_predecessors(inst, key, update);
    skiplist_node_t *target = update[0]->forward[0];

    if (!target || strcmp(target->key, key) != 0) {
        return 1;   // 不存在
    }

    for (int i = 0; i <= inst->level; i++) {
        if (update[i]->forward[i] == target) {
            update[i]->forward[i] = target->forward[i];
        }
    }

    while (inst->level > 0 && inst->header->forward[inst->level] == NULL) {
        inst->level--;
    }

    skiplist_free_node(target);
    inst->size--;

   
    return 0;
}

// 修改键对应的值，成功返回 0，不存在返回 1，错误返回负数
int kvs_skiplist_mod(kvs_skiplist_t *inst, char *key, char *value) {
    if (!inst || !key || !value) return -1;

    skiplist_node_t *current = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    if (current && strcmp(current->key, key) == 0) {
        // 释放旧值，分配新值
        kvs_free(current->value);
        current->value = kvs_malloc(strlen(value) + 1);
        if (!current->value) return -2;
        strcpy(current->value, value);
        return 0;
    }
    return 1;   // 不存在
}

// 检查键是否存在，存在返回 0，不存在返回 1，错误返回 -1
int kvs_skiplist_exist(kvs_skiplist_t *inst, char *key) {
    if (!inst || !key) return -1;
    char *val = kvs_skiplist_get(inst, key);
    return (val != NULL) ? 0 : 1;
}

int kvs_skiplist_expire(kvs_skiplist_t *inst, char *key, long long expire_ms) {
    if (!inst || !key) return -1;
    skiplist_node_t *current = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    if (current && strcmp(current->key, key) == 0) {
        if (expire_ms == 0) {
            return kvs_skiplist_del(inst, key);
        }
        current->expire_ms = expire_ms;
        return 0;
    }
    return 1;   // 不存在
}

long long kvs_skiplist_ttl(kvs_skiplist_t *inst, char *key) {
    if (!inst || !key) return -2;
    skiplist_node_t *current = inst->header;
    for (int i = inst->level; i >= 0; i--) {
        while (current->forward[i] && strcmp(current->forward[i]->key, key) < 0) {
            current = current->forward[i];
        }
    }
    current = current->forward[0];
    if (current && strcmp(current->key, key) == 0) {
        if (current->expire_ms == -1) return -1;
        long long now = kvs_current_time_ms();
        long long ttl = current->expire_ms - now;
        return (ttl > 0) ? ttl : 0;
    }
    return -2;
}

void kvs_skiplist_scan_expired(kvs_skiplist_t *inst) {
    if (!inst) return;
    long long now = kvs_current_time_ms();
    skiplist_node_t *node = inst->header->forward[0];
    while (node) {
        skiplist_node_t *next = node->forward[0];
        if (node->expire_ms != -1 && node->expire_ms <= now) {
            kvs_skiplist_del(inst, node->key);
        }
        node = next;
    }
}