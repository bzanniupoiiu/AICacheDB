#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvstore.h"

#if ENABLE_VECTOR

#define HNSW_NEIGHBOR_CAP (KVS_HNSW_M * 2) //这个就是参数M，允许先最大保留2M，然后裁剪到M

typedef struct candidate {
    int id;//候选向量在inst->items数组中的编号
    float score;//表示候选向量与查询向量的相似度分数，这里采用的是余弦相似度，分数越大，说明两个向量越相似
} candidate_t;//候选结果结构体

kvs_vector_t global_vector_tree = {0};//这是一个全局向量的索引实例
//kvs_vector_t 包含一颗红黑树，vector_entry_t，item_cap，下一个id，入口节点id，maxlevel，
static float *parse_vector(char *vector_str, int dim)//把字符串解析成float向量。
{
    char *copy;
    char *saveptr = NULL;
    char *token;
    float *vec;
    int i = 0;

    if (!vector_str || dim <= 0) {
        return NULL;
    }

    vec = (float *)kvs_malloc(sizeof(float) * dim);
    if (!vec) {
        return NULL;
    }

    copy = (char *)kvs_malloc(strlen(vector_str) + 1);
    if (!copy) {
        kvs_free(vec);
        return NULL;
    }
    strcpy(copy, vector_str);

    token = strtok_r(copy, ",", &saveptr);
    while (token && i < dim) {
        char *end = NULL;
        errno = 0;
        vec[i] = strtof(token, &end);
        if (errno != 0 || end == token || *end != '\0' || !isfinite(vec[i])) {
            kvs_free(copy);
            kvs_free(vec);
            return NULL;
        }
        i++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    kvs_free(copy);

    if (i != dim || token != NULL) {
        kvs_free(vec);
        return NULL;
    }

    return vec;
}

static float vector_norm(float *vec, int dim)//计算当前向量的L2范数
{
    float sum = 0.0f;

    if (!vec || dim <= 0) {
        return 0.0f;
    }

    for (int i = 0; i < dim; i++) {
        sum += vec[i] * vec[i];
    }

    return sqrtf(sum);
}

static float cosine_similarity(float *a, float *b, int dim, float norm_a, float norm_b)//用于计算两个向量的余弦相似度
{
    float dot = 0.0f;

    if (!a || !b || dim <= 0 || norm_a <= 0.0f || norm_b <= 0.0f) {
        return -2.0f;
    }

    for (int i = 0; i < dim; i++) {
        dot += a[i] * b[i];
    }

    return dot / (norm_a * norm_b);
}

//这个函数主要判断候选id是否已经存在了
static int candidate_find(candidate_t *items, int count, int id)//在候选数组中查找id
{
    for (int i = 0; i < count; i++) {
        if (items[i].id == id) {
            return i;
        }
    }
    return -1;
}

//这个函数 在候选集合中，按照相似度降序插入，其实就是维护的一个topk的集合
static void candidate_add_sorted(candidate_t *items, int *count, int cap, int id, float score)
{
    int pos;

    if (!items || !count || cap <= 0 || id < 0) {
        return;
    }

    if (candidate_find(items, *count, id) >= 0) {
        return;
    }

    if (*count >= cap && score <= items[*count - 1].score) {
        return;
    }

    pos = *count;
    if (pos >= cap) {
        pos = cap - 1;
    } else {
        (*count)++;
    }

    while (pos > 0 && items[pos - 1].score < score) {
        items[pos] = items[pos - 1];
        pos--;
    }

    items[pos].id = id;
    items[pos].score = score;
}

//计算某个entry和query的相似度
static float entry_score(vector_entry_t *entry, float *query, int dim, float query_norm)
{
    if (!entry || entry->hnsw_deleted || entry->dim != dim) {
        return -2.0f;
    }
    return cosine_similarity(query, entry->vector, dim, query_norm, entry->norm);
}

//通过id计算余弦相似度
static float id_score(kvs_vector_t *inst, int id, float *query, int dim, float query_norm)
{
    if (!inst || id < 0 || id >= inst->next_id || !inst->items || !inst->items[id]) {
        return -2.0f;
    }
    return entry_score(inst->items[id], query, dim, query_norm);
}
//确保items的数组容量足够
static int ensure_item_capacity(kvs_vector_t *inst)
{
    vector_entry_t **new_items;
    int new_cap;

    if (!inst) {
        return -1;
    }

    if (inst->next_id < inst->item_cap) {
        return 0;
    }

    new_cap = inst->item_cap == 0 ? 128 : inst->item_cap * 2;
    new_items = (vector_entry_t **)kvs_malloc(sizeof(vector_entry_t *) * new_cap);
    if (!new_items) {
        return -1;
    }
    memset(new_items, 0, sizeof(vector_entry_t *) * new_cap);

    if (inst->items && inst->item_cap > 0) {
        memcpy(new_items, inst->items, sizeof(vector_entry_t *) * inst->item_cap);
        kvs_free(inst->items);
    }

    inst->items = new_items;
    inst->item_cap = new_cap;
    return 0;
}

static unsigned int hnsw_rand(kvs_vector_t *inst)//随机数生成器
{
    inst->rand_seed = inst->rand_seed * 1103515245u + 12345u;
    return inst->rand_seed;
}

static int hnsw_random_level(kvs_vector_t *inst)//随机生成hnsw层数，每次大约 50% 概率继续升一层，这个跟tm sklist好像，最高层数是8
{
    int level = 0;

    while (level + 1 < KVS_HNSW_MAX_LEVEL) {
        unsigned int r = hnsw_rand(inst);
        if ((r & 0xffffu) >= 0x8000u) {
            break;
        }
        level++;
    }

    return level;
}

static int hnsw_alloc_neighbors(vector_entry_t *entry)//给entry分配邻居数组
{
    if (!entry) {
        return -1;
    }

    for (int level = 0; level <= entry->hnsw_level && level < KVS_HNSW_MAX_LEVEL; level++) {
        entry->hnsw_neighbors[level] = (int *)kvs_malloc(sizeof(int) * HNSW_NEIGHBOR_CAP);
        if (!entry->hnsw_neighbors[level]) {
            return -1;
        }
        for (int i = 0; i < HNSW_NEIGHBOR_CAP; i++) {
            entry->hnsw_neighbors[level][i] = -1;
        }
        entry->hnsw_neighbor_count[level] = 0;
    }

    return 0;
}

static void hnsw_free_neighbors(vector_entry_t *entry)//每插入一个新节点，都会给这个新节点分配邻居数组
{
    if (!entry) {
        return;
    }

    for (int level = 0; level < KVS_HNSW_MAX_LEVEL; level++) {
        if (entry->hnsw_neighbors[level]) {
            kvs_free(entry->hnsw_neighbors[level]);
            entry->hnsw_neighbors[level] = NULL;
        }
        entry->hnsw_neighbor_count[level] = 0;
    }
}
//创建一个向量节点，一条entry大概包含question/answer/vector/dim/norm/ctime/hnsw_id/hnsw_level/hnsw_deleted/hnsw_neighbors
static vector_entry_t *vector_entry_create(char *question, char *answer, int dim, float *vector)//只是创建一个 vector_entry_t 向量节点对象，但还没有把它插入到 kvs_vector_t 里面。
{
    vector_entry_t *entry;

    if (!question || !answer || !vector || dim <= 0) {
        return NULL;
    }

    entry = (vector_entry_t *)kvs_malloc(sizeof(vector_entry_t));
    if (!entry) {
        return NULL;
    }
    memset(entry, 0, sizeof(*entry));

    entry->question = (char *)kvs_malloc(strlen(question) + 1);
    entry->answer = (char *)kvs_malloc(strlen(answer) + 1);
    if (!entry->question || !entry->answer) {
        if (entry->question) kvs_free(entry->question);
        if (entry->answer) kvs_free(entry->answer);
        kvs_free(entry);
        return NULL;
    }

    strcpy(entry->question, question);
    strcpy(entry->answer, answer);
    entry->vector = vector;
    entry->dim = dim;
    entry->norm = vector_norm(vector, dim);
    entry->ctime = kvs_current_time_ms();
    entry->hnsw_id = -1;
    entry->hnsw_level = 0;
    entry->hnsw_deleted = 0;

    if (entry->norm <= 0.0f) {
        kvs_free(entry->question);
        kvs_free(entry->answer);
        kvs_free(entry);
        return NULL;
    }

    return entry;
}

static void vector_entry_free(vector_entry_t *entry)
{
    if (!entry) {
        return;
    }

    hnsw_free_neighbors(entry);
    if (entry->question) kvs_free(entry->question);
    if (entry->answer) kvs_free(entry->answer);
    if (entry->vector) kvs_free(entry->vector);
    kvs_free(entry);
}

//邻居裁剪
static void hnsw_prune_neighbors(kvs_vector_t *inst, vector_entry_t *entry, int level)//整理 entry 在某一层 level 的邻居列表，只保留最相似的 KVS_HNSW_M 个邻居，其余删掉。
{
    candidate_t tmp[HNSW_NEIGHBOR_CAP];
    int count = 0;

    if (!inst || !entry || level < 0 || level >= KVS_HNSW_MAX_LEVEL ||
        !entry->hnsw_neighbors[level]) {
        return;
    }

    for (int i = 0; i < entry->hnsw_neighbor_count[level]; i++) {
        int nid = entry->hnsw_neighbors[level][i];
        float score = id_score(inst, nid, entry->vector, entry->dim, entry->norm);
        if (score > -2.0f) {
            candidate_add_sorted(tmp, &count, HNSW_NEIGHBOR_CAP, nid, score);
        }
    }

    if (count > KVS_HNSW_M) {
        count = KVS_HNSW_M;
    }

    for (int i = 0; i < HNSW_NEIGHBOR_CAP; i++) {
        entry->hnsw_neighbors[level][i] = i < count ? tmp[i].id : -1;
    }
    entry->hnsw_neighbor_count[level] = count;
}

//添加邻居，建立联系
static void hnsw_add_neighbor(kvs_vector_t *inst, int from_id, int to_id, int level)
{
    vector_entry_t *from;
    int count;
    float to_score;
    int worst_idx = -1;
    float worst_score = FLT_MAX;

    if (!inst || from_id < 0 || to_id < 0 || from_id == to_id ||
        from_id >= inst->next_id || to_id >= inst->next_id ||
        level < 0 || level >= KVS_HNSW_MAX_LEVEL) {
        return;
    }

    from = inst->items[from_id];
    if (!from || from->hnsw_deleted || level > from->hnsw_level ||
        !from->hnsw_neighbors[level]) {
        return;
    }

    count = from->hnsw_neighbor_count[level];
    for (int i = 0; i < count; i++) {
        if (from->hnsw_neighbors[level][i] == to_id) {
            return;
        }
    }

    if (count < HNSW_NEIGHBOR_CAP) {
        from->hnsw_neighbors[level][count] = to_id;
        from->hnsw_neighbor_count[level]++;
        hnsw_prune_neighbors(inst, from, level);
        return;
    }

    to_score = id_score(inst, to_id, from->vector, from->dim, from->norm);
    for (int i = 0; i < count; i++) {
        int nid = from->hnsw_neighbors[level][i];
        float score = id_score(inst, nid, from->vector, from->dim, from->norm);
        if (score < worst_score) {
            worst_score = score;
            worst_idx = i;
        }
    }

    if (worst_idx >= 0 && to_score > worst_score) {
        from->hnsw_neighbors[level][worst_idx] = to_id;
        hnsw_prune_neighbors(inst, from, level);
    }
}

//沿着越来越接近 query 的邻居一路往前走，贪心粗略搜索，只找一个更好的入口点
static int hnsw_greedy(kvs_vector_t *inst, int entry_id, int level, float *query, int dim, float query_norm)
{
    int current = entry_id;
    float current_score = id_score(inst, current, query, dim, query_norm);
    int changed = 1;

    while (changed) {
        vector_entry_t *entry;
        changed = 0;

        if (current < 0 || current >= inst->next_id) {
            break;
        }

        entry = inst->items[current];
        if (!entry || level > entry->hnsw_level || !entry->hnsw_neighbors[level]) {
            break;
        }

        for (int i = 0; i < entry->hnsw_neighbor_count[level]; i++) {
            int nid = entry->hnsw_neighbors[level][i];
            float score = id_score(inst, nid, query, dim, query_norm);
            if (score > current_score) {
                current = nid;
                current_score = score;
                changed = 1;
            }
        }
    }

    return current;
}
//hnsw_search_layer，扩展多个候选节点
// out         输出候选结果的数组
// out_count   输出候选结果数量
// 搜索时允许保留的候选节点数量
static int hnsw_search_layer(kvs_vector_t *inst, float *query, int dim,
                             float query_norm, int entry_id, int ef, int level,
                             candidate_t *out, int *out_count)//在 HNSW 的某一层 level 上，从 entry_id 开始，向周围邻居扩散搜索，最多保留 ef 个最相似的候选节点，最后把这些候选节点返回出去。
{
    candidate_t *candidates;
    unsigned char *visited;//避免重复访问
    int count = 0;

    if (!inst || !query || entry_id < 0 || ef <= 0 || !out || !out_count) {
        return -1;
    }

    candidates = (candidate_t *)kvs_malloc(sizeof(candidate_t) * ef);
    visited = (unsigned char *)kvs_malloc((size_t)inst->next_id);
    if (!candidates || !visited) {
        if (candidates) kvs_free(candidates);
        if (visited) kvs_free(visited);
        return -1;
    }
    memset(visited, 0, (size_t)inst->next_id);

    candidate_add_sorted(candidates, &count, ef, entry_id,
                         id_score(inst, entry_id, query, dim, query_norm));
    visited[entry_id] = 1;

    for (int cursor = 0; cursor < count; cursor++) {
        int id = candidates[cursor].id;
        vector_entry_t *entry = inst->items[id];

        if (!entry || level > entry->hnsw_level || !entry->hnsw_neighbors[level]) {
            continue;
        }

        for (int i = 0; i < entry->hnsw_neighbor_count[level]; i++) {
            int nid = entry->hnsw_neighbors[level][i];
            float score;
            if (nid < 0 || nid >= inst->next_id || visited[nid]) {
                continue;
            }
            visited[nid] = 1;
            score = id_score(inst, nid, query, dim, query_norm);
            if (score > -2.0f) {
                candidate_add_sorted(candidates, &count, ef, nid, score);
            }
        }
    }

    *out_count = count;
    for (int i = 0; i < count; i++) {
        out[i] = candidates[i];
    }

    kvs_free(candidates);
    kvs_free(visited);
    return 0;
}

static int hnsw_insert(kvs_vector_t *inst, vector_entry_t *entry)
{
    int ep;
    int old_max;

    if (!inst || !entry || entry->hnsw_id < 0) {
        return -1;
    }

    if (inst->entry_point < 0) {
        inst->entry_point = entry->hnsw_id;
        inst->max_level = entry->hnsw_level;
        return 0;
    }

    ep = inst->entry_point;
    old_max = inst->max_level;

    for (int level = old_max; level > entry->hnsw_level; level--) {
        ep = hnsw_greedy(inst, ep, level, entry->vector, entry->dim, entry->norm);
    }

    for (int level = entry->hnsw_level < old_max ? entry->hnsw_level : old_max;
         level >= 0;
         level--) {
        candidate_t found[KVS_HNSW_EF_CONSTRUCTION];
        int found_count = 0;
        int links;

        if (hnsw_search_layer(inst,
                              entry->vector,
                              entry->dim,
                              entry->norm,
                              ep,
                              KVS_HNSW_EF_CONSTRUCTION,
                              level,
                              found,
                              &found_count) != 0) {
            return -1;
        }

        links = found_count < KVS_HNSW_M ? found_count : KVS_HNSW_M;
        for (int i = 0; i < links; i++) {
            hnsw_add_neighbor(inst, entry->hnsw_id, found[i].id, level);
            hnsw_add_neighbor(inst, found[i].id, entry->hnsw_id, level);
        }

        if (found_count > 0) {
            ep = found[0].id;
        }
    }

    if (entry->hnsw_level > inst->max_level) {
        inst->max_level = entry->hnsw_level;
        inst->entry_point = entry->hnsw_id;
    }

    return 0;
}
//调用hnsw_insert，把向量节点插入HNSW
static int vector_attach_to_hnsw(kvs_vector_t *inst, vector_entry_t *entry)
{
    if (!inst || !entry || ensure_item_capacity(inst) != 0) {
        return -1;
    }

    entry->hnsw_id = inst->next_id;
    entry->hnsw_level = hnsw_random_level(inst);

    if (hnsw_alloc_neighbors(entry) != 0) {
        return -1;
    }

    inst->items[inst->next_id++] = entry;
    return hnsw_insert(inst, entry);
}
//只释放红黑树节点，不释放向量节点
static void free_tree_nodes_only(kvs_vector_t *inst)
{
    while (inst && inst->tree.root != inst->tree.nil) {
        rbtree_node *mini = rbtree_mini(&inst->tree, inst->tree.root);
        rbtree_node *cur = rbtree_delete(&inst->tree, mini);
        if (cur) {
            if (cur->key) kvs_free(cur->key);
            kvs_free(cur);
        }
    }

    if (inst && inst->tree.nil) {
        kvs_free(inst->tree.nil);
        inst->tree.nil = NULL;
    }
}

int kvs_vector_create(kvs_vector_t *inst)
{
    if (!inst) {
        return -1;
    }

    memset(inst, 0, sizeof(*inst));
    if (kvs_rbtree_create(&inst->tree) != 0) {
        return -1;
    }

    inst->entry_point = -1;
    inst->max_level = 0;
    inst->rand_seed = 0x9e3779b9u;
    return 0;
}

void kvs_vector_destroy(kvs_vector_t *inst)
{
    if (!inst) {
        return;
    }

    free_tree_nodes_only(inst);

    if (inst->items) {
        for (int i = 0; i < inst->next_id; i++) {
            if (inst->items[i]) {
                vector_entry_free(inst->items[i]);
                inst->items[i] = NULL;
            }
        }
        kvs_free(inst->items);
    }

    memset(inst, 0, sizeof(*inst));
    inst->entry_point = -1;
}

int kvs_vector_set(kvs_vector_t *inst, char *question, char *answer, int dim, char *vector_str)
{
    vector_entry_t *entry;
    rbtree_node *old;
    float *vec;

    if (!inst || !question || !answer || !vector_str || dim <= 0) {
        return -1;
    }

    vec = parse_vector(vector_str, dim);
    if (!vec) {
        return -2;
    }

    entry = vector_entry_create(question, answer, dim, vec);
    if (!entry) {
        kvs_free(vec);
        return -2;
    }

    if (vector_attach_to_hnsw(inst, entry) != 0) {//插入向量图
        vector_entry_free(entry);
        return -2;
    }

    old = rbtree_search(&inst->tree, question);
    if (old != inst->tree.nil) {
        vector_entry_t *old_entry = (vector_entry_t *)old->value;
        if (old_entry) {
            old_entry->hnsw_deleted = 1;
        }
        old->value = entry;
        return 0;
    }

    rbtree_node *node = (rbtree_node *)kvs_malloc(sizeof(rbtree_node));
    if (!node) {
        entry->hnsw_deleted = 1;
        return -2;
    }
    memset(node, 0, sizeof(*node));

    node->key = (char *)kvs_malloc(strlen(question) + 1);
    if (!node->key) {
        kvs_free(node);
        entry->hnsw_deleted = 1;
        return -2;
    }

    strcpy(node->key, question);
    node->value = entry;
    node->expire_ms = -1;

    rbtree_insert(&inst->tree, node);
    inst->count++;
    return 0;
}

vector_entry_t *kvs_vector_get_by_key(kvs_vector_t *inst, char *question)
{
    rbtree_node *node;

    if (!inst || !question) {
        return NULL;
    }

    node = rbtree_search(&inst->tree, question);
    if (node == inst->tree.nil) {
        return NULL;
    }

    return (vector_entry_t *)node->value;
}
//暴力兜底 ，，
static int exact_search_all(kvs_vector_t *inst, float *query_vector, int dim,
                            float query_norm, int topk, float threshold,
                            candidate_t *top, int *top_count)
{
    if (!inst || !top || !top_count) {
        return -1;
    }

    for (int id = 0; id < inst->next_id; id++) {
        float score = id_score(inst, id, query_vector, dim, query_norm);
        if (score >= threshold) {
            candidate_add_sorted(top, top_count, topk, id, score);
        }
    }

    return 0;
}

int kvs_vector_search(kvs_vector_t *inst, int dim, char *vector_str, int topk,
                      float threshold, vector_search_result_t **results, int *out_count)
{
    float *query_vector;
    float query_norm;
    candidate_t candidates[KVS_HNSW_EF_SEARCH];
    candidate_t *top = NULL;
    int candidate_count = 0;
    int top_count = 0;
    int ep;

    if (!results || !out_count) {
        return -1;
    }
    *results = NULL;
    *out_count = 0;

    if (!inst || dim <= 0 || !vector_str || topk <= 0) {
        return -1;
    }

    if (topk > 128) {
        topk = 128;
    }

    query_vector = parse_vector(vector_str, dim);
    if (!query_vector) {
        return -2;
    }
    query_norm = vector_norm(query_vector, dim);
    if (query_norm <= 0.0f) {
        kvs_free(query_vector);
        return -2;
    }

    top = (candidate_t *)kvs_malloc(sizeof(candidate_t) * topk);
    if (!top) {
        kvs_free(query_vector);
        return -2;
    }

    if (inst->entry_point >= 0 && inst->next_id > 0) {
        ep = inst->entry_point;
        for (int level = inst->max_level; level > 0; level--) {
            ep = hnsw_greedy(inst, ep, level, query_vector, dim, query_norm);
        }

        hnsw_search_layer(inst,
                          query_vector,
                          dim,
                          query_norm,
                          ep,
                          KVS_HNSW_EF_SEARCH,
                          0,
                          candidates,
                          &candidate_count);
    }

    for (int i = 0; i < candidate_count; i++) {
        float score = id_score(inst, candidates[i].id, query_vector, dim, query_norm);
        if (score >= threshold) {
            candidate_add_sorted(top, &top_count, topk, candidates[i].id, score);
        }
    }

    // if (candidate_count == 0 || top_count == 0) {//全量查询兜底
    //     exact_search_all(inst, query_vector, dim, query_norm, topk, threshold, top, &top_count);
    // }

    kvs_free(query_vector);

    if (top_count <= 0) {
        kvs_free(top);
        return 0;
    }

    *results = (vector_search_result_t *)kvs_malloc(sizeof(vector_search_result_t) * top_count);
    if (!*results) {
        kvs_free(top);
        return -2;
    }

    for (int i = 0; i < top_count; i++) {
        vector_entry_t *entry = inst->items[top[i].id];
        (*results)[i].question = entry->question;
        (*results)[i].answer = entry->answer;
        (*results)[i].score = top[i].score;
    }

    *out_count = top_count;
    kvs_free(top);
    return top_count;
}

void kvs_vector_search_results_free(vector_search_result_t *results)
{
    if (results) {
        kvs_free(results);
    }
}

int kvs_vector_get_by_vector(kvs_vector_t *inst, int dim, char *vector_str,
                             float threshold, vector_get_result_t *result)
{
    vector_search_result_t *results = NULL;
    int count = 0;
    int ret;

    if (!result) {
        return -1;
    }

    ret = kvs_vector_search(inst, dim, vector_str, 1, threshold, &results, &count);
    if (ret < 0) {
        return ret;
    }

    if (count <= 0 || !results) {
        return 0;
    }

    result->question = results[0].question;
    result->answer = results[0].answer;
    result->score = results[0].score;
    kvs_vector_search_results_free(results);
    return 1;
}

#endif
