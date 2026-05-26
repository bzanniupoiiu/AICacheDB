

#include "kvstore.h"

#include <sys/time.h>



kvs_array_t global_array = {0};

int kvs_array_create(kvs_array_t *inst) {

	if (!inst) return -1;
	if (inst->table) {
		printf("table has alloc\n");
		return -1;
	}	
	inst->table = kvs_malloc(KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));
	if (!inst->table) { return -1; }
    memset(inst->table, 0, KVS_ARRAY_SIZE * sizeof(kvs_array_item_t));

	inst->total = 0;

	return 0;
}

void kvs_array_destory(kvs_array_t *inst) {

	if (!inst) return;
    if (inst->table) {
        
        for (int i = 0; i < inst->total; i++) {
            if (inst->table[i].key) {
                kvs_free(inst->table[i].key);
                inst->table[i].key = NULL;
            }
            if (inst->table[i].value) {
                kvs_free(inst->table[i].value);
                inst->table[i].value = NULL;
            }
        }
        
        kvs_free(inst->table);
        inst->table = NULL;
    }
    inst->total = 0;

}


/*
 * @return: <0, error; =0, success; >0, exist
 */

int kvs_array_set(kvs_array_t *inst, char *key, char *value,long long expire_ms) {

	if (!inst || !key || !value) return -1;
    if (inst->total >= KVS_ARRAY_SIZE) return -1;

    // 查找是否存在
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            // 覆盖值
            kvs_free(inst->table[i].value);
            inst->table[i].value = kvs_malloc(strlen(value) + 1);
            if (!inst->table[i].value) return -2;
            strcpy(inst->table[i].value, value);
            inst->table[i].expire_ms = expire_ms;
            return 0;   // 表示更新成功，但调用方可能需区分（返回 1 表示存在？）
        }
    }

    // 新增
    char *kcopy = kvs_malloc(strlen(key) + 1);

    if (!kcopy) return -2;

    strcpy(kcopy, key);

    char *vcopy = kvs_malloc(strlen(value) + 1);
    if (!vcopy) {
        kvs_free(kcopy);
        return -2;
    }
    strcpy(vcopy, value);

    inst->table[inst->total].key = kcopy;
    inst->table[inst->total].value = vcopy;
    inst->table[inst->total].expire_ms = expire_ms;
    inst->total++;

    return 0;   // 插入成功
}

char* kvs_array_get(kvs_array_t *inst, char *key) {
    if (!inst || !key) return NULL;

    long long now = kvs_current_time_ms();
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            if (inst->table[i].expire_ms != -1 && inst->table[i].expire_ms <= now) {
                kvs_free(inst->table[i].key);
                kvs_free(inst->table[i].value);

                inst->table[i] = inst->table[inst->total - 1];
                inst->total--;

                return NULL;
            }
            return inst->table[i].value;
        }
    }
    return NULL;
}


int kvs_array_expire(kvs_array_t *inst, char *key, long long expire_ms) {
    if (!inst || !key) return -1;
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            if (expire_ms == 0) {   // 立即过期
                return kvs_array_del(inst, key);
            }
            inst->table[i].expire_ms = expire_ms;
            return 0;
        }
    }
    return 1;   // key 不存在
}

long long kvs_array_ttl(kvs_array_t *inst, char *key) {
    if (!inst || !key) return -2;   // error
    long long now = kvs_current_time_ms();
    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            if (inst->table[i].expire_ms == -1) return -1;   // 永不过期
            long long ttl = inst->table[i].expire_ms - now;
            return (ttl > 0) ? ttl : 0;
        }
    }
    return -2;   // key 不存在
}

void kvs_array_scan_expired(kvs_array_t *inst) {
    if (!inst) return;
    long long now = kvs_current_time_ms();
    // 倒序遍历，避免删除后索引错位
    for (int i = inst->total - 1; i >= 0; i--) {
        if (inst->table[i].key && inst->table[i].expire_ms != -1 && inst->table[i].expire_ms <= now) {
            kvs_array_del(inst, inst->table[i].key);
        }
    }
}

/*
 * @return < 0, error;  =0,  success; >0, no exist
 */

int kvs_array_del(kvs_array_t *inst, char *key) {
    if (!inst || !key) return -1;

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            kvs_free(inst->table[i].key);
            kvs_free(inst->table[i].value);
            // 移动最后一个元素填充删除位置
            if (i < inst->total - 1) {
                inst->table[i] = inst->table[inst->total - 1];
            }
            inst->table[inst->total - 1].key = NULL;
            inst->table[inst->total - 1].value = NULL;

            inst->total--;          
            return 0;
        }
    }
    return 1;   // 未找到
}

/*
 * @return : < 0, error; =0, success; >0, no exist 
 */

int kvs_array_mod(kvs_array_t *inst, char *key, char *value) {

	if (inst == NULL || key == NULL || value == NULL) return -1;
// error: > 1024
	if (inst->total == 0) {
		return KVS_ARRAY_SIZE;
	}
	
	int i = 0;
	for (i = 0;i < inst->total;i ++) {

		if (inst->table[i].key == NULL) {
			continue;
		}

		if (strcmp(inst->table[i].key, key) == 0) {

			kvs_free(inst->table[i].value);

			char *kvalue = kvs_malloc(strlen(value) + 1);
			if (kvalue == NULL) return -2;
			memset(kvalue, 0, strlen(value) + 1);
			strncpy(kvalue, value, strlen(value));

			inst->table[i].value = kvalue;

			return 0;
		}

	}

	return i;
}


/*
 * @return 0: exist, 1: no exist
 */
int kvs_array_exist(kvs_array_t *inst, char *key) {
    if (!inst || !key) return -1;

    long long now = kvs_current_time_ms();

    for (int i = 0; i < inst->total; i++) {
        if (inst->table[i].key && strcmp(inst->table[i].key, key) == 0) {
            if (inst->table[i].expire_ms != -1 &&
                inst->table[i].expire_ms <= now) {
                return 1; // 视为不存在
            }
            return 0;
        }
    }
    return 1;
}
