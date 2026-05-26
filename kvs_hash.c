


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>


#include "kvstore.h"


kvs_hash_t global_hash;


static int _hash(char *key, int size) {

	if (!key) return -1;

	int sum = 0;
	int i = 0;

	while (key[i] != 0) {
		sum += key[i];
		i ++;
	}

	return sum % size;

}

hashnode_t *_create_node(char *key, char *value) {

	hashnode_t *node = (hashnode_t*)kvs_malloc(sizeof(hashnode_t));
	if (!node) return NULL;
	
#if ENABLE_KEY_POINTER
	char *kcopy = kvs_malloc(strlen(key) + 1);
	if (kcopy == NULL) return NULL;
	memset(kcopy, 0, strlen(key) + 1);
	strncpy(kcopy, key, strlen(key));

	node->key = kcopy;

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (!kvalue) {
        kvs_free(kcopy);
        kvs_free(node);
        return NULL;
    }
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));

	node->value = kvalue;
	
#else
	strncpy(node->key, key, MAX_KEY_LEN);
	strncpy(node->value, value, MAX_VALUE_LEN);
#endif
	node->next = NULL;

	return node;
}



int kvs_hash_create(kvs_hash_t *hash) {
    if (!hash) return -1;

    hash->nodes = (hashnode_t**)kvs_malloc(sizeof(hashnode_t*) * MAX_TABLE_SIZE);
    if (!hash->nodes) {
        fprintf(stderr, "kvs_hash_create: failed to allocate nodes array\n");
        return -1;
    }

    hash->max_slots = MAX_TABLE_SIZE;
    hash->count = 0;

    
    return 0;
}


void kvs_hash_destory(kvs_hash_t *hash) {

	if (!hash) return;

	int i = 0;
	for (i = 0;i < hash->max_slots;i ++) {
		hashnode_t *node = hash->nodes[i];

		while (node != NULL) { // error

			hashnode_t *tmp = node;
			node = node->next;
			hash->nodes[i] = node;
			
			kvs_free(tmp->key);
            kvs_free(tmp->value);
            kvs_free(tmp);
			
		}
	}

	kvs_free(hash->nodes);
	
}

// 5 + 2

// mp
int kvs_hash_set(kvs_hash_t *hash, char *key, char *value, long long expire_ms) {
    if (!hash || !key || !value) return -1;

    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *node = hash->nodes[idx];
    // 检查是否存在并更新
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            // 更新值
            kvs_free(node->value);
            node->value = kvs_malloc(strlen(value) + 1);
            if (!node->value) return -2;
            strcpy(node->value, value);
            node->expire_ms = expire_ms;
            return 0;   // 已存在
        }
        node = node->next;
    }

    // 创建新节点
    hashnode_t *new_node = _create_node(key, value);
    if (!new_node) return -2;
    new_node->expire_ms = expire_ms;   // 设置过期时间
    new_node->next = hash->nodes[idx];
    hash->nodes[idx] = new_node;
    hash->count++;
    return 0;
}

char* kvs_hash_get(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return NULL;
    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *node = hash->nodes[idx];
    long long now = kvs_current_time_ms();

    hashnode_t *prev = NULL;
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            if (node->expire_ms != -1 && node->expire_ms <= now) {
                // 惰性删除
                if (prev == NULL) {
                    hash->nodes[idx] = node->next;
                } else {
                    prev->next = node->next;
                }
                kvs_free(node->key);
                kvs_free(node->value);
                kvs_free(node);
                hash->count--;
                return NULL;
            }
            return node->value;
        }
        prev = node;
        node = node->next;
    }
    return NULL;
}


int kvs_hash_mod(kvs_hash_t *hash, char *key, char *value) {

	if (!hash || !key) return -1;
    

	int idx = _hash(key, MAX_TABLE_SIZE);

	hashnode_t *node = hash->nodes[idx];

    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            break;
        }
        node = node->next;
    }

    if (!node) return 1;

    long long now = kvs_current_time_ms();
    if (node->expire_ms != -1 && node->expire_ms <= now) {
        kvs_hash_del(hash, key);
        return 1;
    }
	// node --> 
	kvs_free(node->value);

	char *kvalue = kvs_malloc(strlen(value) + 1);
	if (kvalue == NULL) return -2;
	memset(kvalue, 0, strlen(value) + 1);
	strncpy(kvalue, value, strlen(value));

	node->value = kvalue;
	
	return 0;
}

int kvs_hash_count(kvs_hash_t *hash) {
	return hash->count;
}

int kvs_hash_del(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return -2;
    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *head = hash->nodes[idx];
    if (head == NULL) return 1;   // 不存在

    // 头节点
    if (strcmp(head->key, key) == 0) {
        hashnode_t *tmp = head->next;
        kvs_free(head->key);
        kvs_free(head->value);
        kvs_free(head);
        hash->nodes[idx] = tmp;
        hash->count--;

        
        return 0;
    }

    // 非头节点
    hashnode_t *cur = head;
    while (cur->next != NULL) {
        if (strcmp(cur->next->key, key) == 0) {
            hashnode_t *tmp = cur->next;
            cur->next = tmp->next;
            kvs_free(tmp->key);
            kvs_free(tmp->value);
            kvs_free(tmp);
            hash->count--;

            
            return 0;
        }
        cur = cur->next;
    }
    return 1;   // 未找到
}


int kvs_hash_exist(kvs_hash_t *hash, char *key) {

	if (!hash || !key) return -1;

    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *node = hash->nodes[idx];
    long long now = kvs_current_time_ms();

    while (node) {
        if (strcmp(node->key, key) == 0) {
            if (node->expire_ms != -1 && node->expire_ms <= now)
                return 1;
            return 0;
        }
        node = node->next;
    }
    return 1;
	
}

int kvs_hash_expire(kvs_hash_t *hash, char *key, long long expire_ms) {
    if (!hash || !key) return -1;
    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *node = hash->nodes[idx];
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            if (expire_ms == 0) {
                return kvs_hash_del(hash, key);
            }
            node->expire_ms = expire_ms;
            return 0;
        }
        node = node->next;
    }
    return 1;   // 不存在
}

long long kvs_hash_ttl(kvs_hash_t *hash, char *key) {
    if (!hash || !key) return -2;
    int idx = _hash(key, MAX_TABLE_SIZE);
    hashnode_t *node = hash->nodes[idx];
    while (node != NULL) {
        if (strcmp(node->key, key) == 0) {
            if (node->expire_ms == -1) return -1;
            long long now = kvs_current_time_ms();
            long long ttl = node->expire_ms - now;
            return (ttl > 0) ? ttl : 0;
        }
        node = node->next;
    }
    return -2;
}

void kvs_hash_scan_expired(kvs_hash_t *hash) {
    if (!hash) return;
    long long now = kvs_current_time_ms();
    for (int i = 0; i < hash->max_slots; i++) {
        hashnode_t *node = hash->nodes[i];
        hashnode_t *prev = NULL;
        while (node != NULL) {
            if (node->expire_ms != -1 && node->expire_ms <= now) {
                // 删除节点
                hashnode_t *to_del = node;
                if (prev == NULL) {
                    hash->nodes[i] = node->next;
                    node = hash->nodes[i];
                } else {
                    prev->next = node->next;
                    node = node->next;
                }
                kvs_free(to_del->key);
                kvs_free(to_del->value);
                kvs_free(to_del);
                hash->count--;
            } else {
                prev = node;
                node = node->next;
            }
        }
    }
}



