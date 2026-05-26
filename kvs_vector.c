#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "kvstore.h"

#if ENABLE_VECTOR

kvs_vector_t global_vector_tree = {0};

/*
 * 把 "0.1,0.2,-0.3" 解析成 float 数组
 */
static float *parse_vector(char *vector_str, int dim) {
    if (!vector_str || dim <= 0) {
        return NULL;
    }

    float *vec = (float *)kvs_malloc(sizeof(float) * dim);
    if (!vec) {
        return NULL;
    }

    char *copy = (char *)kvs_malloc(strlen(vector_str) + 1);
    if (!copy) {
        kvs_free(vec);
        return NULL;
    }

    strcpy(copy, vector_str);

    int i = 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);

    while (token && i < dim) {
        vec[i++] = (float)atof(token);
        token = strtok_r(NULL, ",", &saveptr);
    }

    kvs_free(copy);

    if (i != dim) {
        kvs_free(vec);
        return NULL;
    }

    return vec;
}

/*
 * 计算向量模长
 */
static float vector_norm(float *vec, int dim) {
    if (!vec || dim <= 0) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (int i = 0; i < dim; i++) {
        sum += vec[i] * vec[i];
    }

    return sqrtf(sum);
}

/*
 * 计算余弦相似度
 */
static float cosine_similarity(
    float *a,
    float *b,
    int dim,
    float norm_a,
    float norm_b
) 
{
    if (!a || !b || dim <= 0 || norm_a == 0.0f || norm_b == 0.0f) {
        return 0.0f;
    }

    float dot = 0.0f;
    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }

    return dot / (norm_a * norm_b);
}

static vector_entry_t *vector_entry_create(
    char *question,
    char *answer,
    int dim,
    float *vector
) 
{
    if (!question || !answer || !vector || dim <= 0) {
        return NULL;
    }

    vector_entry_t *entry = (vector_entry_t *)kvs_malloc(sizeof(vector_entry_t));
    if (!entry) {
        return NULL;
    }

    memset(entry, 0, sizeof(vector_entry_t));

    entry->question = (char *)kvs_malloc(strlen(question) + 1);
    if (!entry->question) {
        kvs_free(entry);
        return NULL;
    }
    strcpy(entry->question, question);

    entry->answer = (char *)kvs_malloc(strlen(answer) + 1);
    if (!entry->answer) {
        kvs_free(entry->question);
        kvs_free(entry);
        return NULL;
    }
    strcpy(entry->answer, answer);

    entry->vector = vector;
    entry->dim = dim;
    entry->norm = vector_norm(vector, dim);
    entry->ctime = kvs_current_time_ms();

    return entry;
}

static void vector_entry_free(vector_entry_t *entry) {
    if (!entry) {
        return;
    }

    if (entry->question) {
        kvs_free(entry->question);
    }

    if (entry->answer) {
        kvs_free(entry->answer);
    }

    if (entry->vector) {
        kvs_free(entry->vector);
    }

    kvs_free(entry);
}

int kvs_vector_create(kvs_vector_t *inst) {
    if (!inst) {
        return -1;
    }

    memset(inst, 0, sizeof(kvs_vector_t));

    if (kvs_rbtree_create(&inst->tree) != 0) {
        return -1;
    }

    inst->count = 0;
    return 0;
}

/*
 * 不能直接调用 kvs_rbtree_destory
 * 因为普通 rbtree 的 value 是 char*
 * 这里的 value 是 vector_entry_t*
 */
void kvs_vector_destroy(kvs_vector_t *inst) {
    if (!inst) {
        return;
    }

    while (inst->tree.root != inst->tree.nil) {
        rbtree_node *mini = rbtree_mini(&inst->tree, inst->tree.root);
        rbtree_node *cur = rbtree_delete(&inst->tree, mini);

        if (cur) {
            if (cur->key) {
                kvs_free(cur->key);
            }

            if (cur->value) {
                vector_entry_free((vector_entry_t *)cur->value);
            }

            kvs_free(cur);
        }
    }

    if (inst->tree.nil) {
        kvs_free(inst->tree.nil);
        inst->tree.nil = NULL;
    }

    inst->count = 0;
}

/*
 * VSET 底层实现：
 * question -> vector_entry_t
 */
int kvs_vector_set(
    kvs_vector_t *inst,
    char *question,
    char *answer,
    int dim,
    char *vector_str
)
{
    if (!inst || !question || !answer || !vector_str || dim <= 0) {
        return -1;
    }

    float *vec = parse_vector(vector_str, dim);
    if (!vec) {
        return -2;
    }

    /*
     * key 已存在：更新 value
     */
    rbtree_node *old = rbtree_search(&inst->tree, question);
    if (old != inst->tree.nil) {
        vector_entry_t *old_entry = (vector_entry_t *)old->value;

        vector_entry_t *new_entry = vector_entry_create(question, answer, dim, vec);
        if (!new_entry) {
            kvs_free(vec);
            return -2;
        }

        if (old_entry) {
            vector_entry_free(old_entry);
        }

        old->value = new_entry;
        return 0;
    }

    /*
     * key 不存在：创建新红黑树节点
     */
    vector_entry_t *entry = vector_entry_create(question, answer, dim, vec);
    if (!entry) {
        kvs_free(vec);
        return -2;
    }

    rbtree_node *node = (rbtree_node *)kvs_malloc(sizeof(rbtree_node));
    if (!node) {
        vector_entry_free(entry);
        return -2;
    }

    memset(node, 0, sizeof(rbtree_node));

    node->key = (char *)kvs_malloc(strlen(question) + 1);
    if (!node->key) {
        kvs_free(node);
        vector_entry_free(entry);
        return -2;
    }

    strcpy(node->key, question);
    node->value = entry;
    node->expire_ms = -1;

    rbtree_insert(&inst->tree, node);
    inst->count++;

    return 0;
}

/*
 * 精确查询：
 * VGET question
 */
vector_entry_t *kvs_vector_get_by_key(
    kvs_vector_t *inst,
    char *question
) 
{
    if (!inst || !question) {
        return NULL;
    }

    rbtree_node *node = rbtree_search(&inst->tree, question);
    if (node == inst->tree.nil) {
        return NULL;
    }

    return (vector_entry_t *)node->value;
}

typedef struct vector_search_ctx {
    float *query_vector;
    int dim;
    float query_norm;
    float threshold;

    vector_entry_t *best_entry;
    float best_score;
} vector_search_ctx_t;

/*
 * 中序遍历整棵 vector 红黑树
 */
static void vector_search_dfs(
    kvs_vector_t *inst,
    rbtree_node *node,
    vector_search_ctx_t *ctx
) 
{
    if (!inst || !ctx || node == inst->tree.nil) {
        return;
    }

    vector_search_dfs(inst, node->left, ctx);

    vector_entry_t *entry = (vector_entry_t *)node->value;
    if (entry && entry->dim == ctx->dim) {
        float score = cosine_similarity(
            ctx->query_vector,
            entry->vector,
            ctx->dim,
            ctx->query_norm,
            entry->norm
        );

        if (score > ctx->best_score) {
            ctx->best_score = score;
            ctx->best_entry = entry;
        }
    }

    vector_search_dfs(inst, node->right, ctx);
}

/*
 * 语义查询：
 * VGET dim vector threshold
 */
int kvs_vector_get_by_vector(
    kvs_vector_t *inst,
    int dim,
    char *vector_str,
    float threshold,
    vector_get_result_t *result
) 
{
    if (!inst || dim <= 0 || !vector_str || !result) {
        return -1;
    }

    float *query_vector = parse_vector(vector_str, dim);
    if (!query_vector) {
        return -2;
    }

    vector_search_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.query_vector = query_vector;
    ctx.dim = dim;
    ctx.query_norm = vector_norm(query_vector, dim);
    ctx.threshold = threshold;
    ctx.best_entry = NULL;
    ctx.best_score = -2.0f;

    vector_search_dfs(inst, inst->tree.root, &ctx);

    kvs_free(query_vector);

    if (ctx.best_entry && ctx.best_score >= threshold) {
        result->question = ctx.best_entry->question;
        result->answer = ctx.best_entry->answer;
        result->score = ctx.best_score;
        return 1;
    }

    return 0;
}

#endif