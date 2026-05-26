


#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "kvstore.h"

rbtree_node *rbtree_mini(rbtree *T, rbtree_node *x) {
	while (x->left != T->nil) {
		x = x->left;
	}
	return x;
}

rbtree_node *rbtree_maxi(rbtree *T, rbtree_node *x) {
	while (x->right != T->nil) {
		x = x->right;
	}
	return x;
}

rbtree_node *rbtree_successor(rbtree *T, rbtree_node *x) {
	rbtree_node *y = x->parent;

	if (x->right != T->nil) {
		return rbtree_mini(T, x->right);
	}

	while ((y != T->nil) && (x == y->right)) {
		x = y;
		y = y->parent;
	}
	return y;
}


void rbtree_left_rotate(rbtree *T, rbtree_node *x) {

	rbtree_node *y = x->right;  // x  --> y  ,  y --> x,   right --> left,  left --> right

	x->right = y->left; //1 1
	if (y->left != T->nil) { //1 2
		y->left->parent = x;
	}

	y->parent = x->parent; //1 3
	if (x->parent == T->nil) { //1 4
		T->root = y;
	} else if (x == x->parent->left) {
		x->parent->left = y;
	} else {
		x->parent->right = y;
	}

	y->left = x; //1 5
	x->parent = y; //1 6
}


void rbtree_right_rotate(rbtree *T, rbtree_node *y) {

	rbtree_node *x = y->left;

	y->left = x->right;
	if (x->right != T->nil) {
		x->right->parent = y;
	}

	x->parent = y->parent;
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->right) {
		y->parent->right = x;
	} else {
		y->parent->left = x;
	}

	x->right = y;
	y->parent = x;
}

void rbtree_insert_fixup(rbtree *T, rbtree_node *z) {

	while (z->parent->color == RED) { //z ---> RED
		if (z->parent == z->parent->parent->left) {
			rbtree_node *y = z->parent->parent->right;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {

				if (z == z->parent->right) {
					z = z->parent;
					rbtree_left_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_right_rotate(T, z->parent->parent);
			}
		}else {
			rbtree_node *y = z->parent->parent->left;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;

				z = z->parent->parent; //z --> RED
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					rbtree_right_rotate(T, z);
				}

				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				rbtree_left_rotate(T, z->parent->parent);
			}
		}
		
	}

	T->root->color = BLACK;
}


void rbtree_insert(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->root;

	while (x != T->nil) {
		y = x;
#if ENABLE_KEY_CHAR

		if (strcmp(z->key, x->key) < 0) {
			x = x->left;
		} else if (strcmp(z->key, x->key) > 0) {
			x = x->right;
		} else {
			return ;
		}

#else
		if (z->key < x->key) {
			x = x->left;
		} else if (z->key > x->key) {
			x = x->right;
		} else { //Exist
			return ;
		}
#endif
	}

	z->parent = y;
	if (y == T->nil) {
		T->root = z;
#if ENABLE_KEY_CHAR
	} else if (strcmp(z->key, y->key) < 0) {
#else
	} else if (z->key < y->key) {
#endif
		y->left = z;
	} else {
		y->right = z;
	}

	z->left = T->nil;
	z->right = T->nil;
	z->color = RED;

	rbtree_insert_fixup(T, z);
}

void rbtree_delete_fixup(rbtree *T, rbtree_node *x) {

	while ((x != T->root) && (x->color == BLACK)) {
		if (x == x->parent->left) {

			rbtree_node *w= x->parent->right;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;

				rbtree_left_rotate(T, x->parent);
				w = x->parent->right;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->right->color == BLACK) {
					w->left->color = BLACK;
					w->color = RED;
					rbtree_right_rotate(T, w);
					w = x->parent->right;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->right->color = BLACK;
				rbtree_left_rotate(T, x->parent);

				x = T->root;
			}

		} else {

			rbtree_node *w = x->parent->left;
			if (w->color == RED) {
				w->color = BLACK;
				x->parent->color = RED;
				rbtree_right_rotate(T, x->parent);
				w = x->parent->left;
			}

			if ((w->left->color == BLACK) && (w->right->color == BLACK)) {
				w->color = RED;
				x = x->parent;
			} else {

				if (w->left->color == BLACK) {
					w->right->color = BLACK;
					w->color = RED;
					rbtree_left_rotate(T, w);
					w = x->parent->left;
				}

				w->color = x->parent->color;
				x->parent->color = BLACK;
				w->left->color = BLACK;
				rbtree_right_rotate(T, x->parent);

				x = T->root;
			}

		}
	}

	x->color = BLACK;
}

rbtree_node *rbtree_delete(rbtree *T, rbtree_node *z) {

	rbtree_node *y = T->nil;
	rbtree_node *x = T->nil;

	if ((z->left == T->nil) || (z->right == T->nil)) {
		y = z;
	} else {
		y = rbtree_successor(T, z);
	}

	if (y->left != T->nil) {
		x = y->left;
	} else if (y->right != T->nil) {
		x = y->right;
	}

	x->parent = y->parent;
	if (y->parent == T->nil) {
		T->root = x;
	} else if (y == y->parent->left) {
		y->parent->left = x;
	} else {
		y->parent->right = x;
	}

	if (y != z) {
#if ENABLE_KEY_CHAR

		void *tmp = z->key;
		z->key = y->key;
		y->key = tmp;

		tmp = z->value;
		z->value= y->value;
		y->value = tmp;

#else
		z->key = y->key;
		z->value = y->value;
#endif
	}

	if (y->color == BLACK) {
		rbtree_delete_fixup(T, x);
	}

	return y;
}

rbtree_node *rbtree_search(rbtree *T, KEY_TYPE key) {

	rbtree_node *node = T->root;
	while (node != T->nil) {
#if ENABLE_KEY_CHAR

		if (strcmp(key, node->key) < 0) {
			node = node->left;
		} else if (strcmp(key, node->key) > 0) {
			node = node->right;
		} else {
			return node;
		}

#else
		if (key < node->key) {
			node = node->left;
		} else if (key > node->key) {
			node = node->right;
		} else {
			return node;
		}	
#endif
	}
	return T->nil;
}


void rbtree_traversal(rbtree *T, rbtree_node *node) {
	if (node != T->nil) {
		rbtree_traversal(T, node->left);
#if ENABLE_KEY_CHAR
		printf("key:%s, value:%s\n", node->key, (char *)node->value);
#else
		printf("key:%d, color:%d\n", node->key, node->color);
#endif
		rbtree_traversal(T, node->right);
	}
}



// ____________________________________________________________________________API
typedef struct _rbtree kvs_rbtree_t; 

kvs_rbtree_t global_rbtree;

// 5 + 2
int kvs_rbtree_create(kvs_rbtree_t *inst) {

	if (inst == NULL) return 1;

	inst->nil = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
	if (!inst->nil) return -1;

	inst->nil->color = BLACK;
	inst->nil->left = inst->nil;
	inst->nil->right = inst->nil;
	inst->nil->parent = inst->nil;

	inst->root = inst->nil;

	return 0;

}

void kvs_rbtree_destory(kvs_rbtree_t *inst) {
    if (inst == NULL) return;

    // 正确的销毁方式：中序遍历删除所有节点
    while (inst->root != inst->nil) {
        // 找到最小节点（或任意节点）删除
        rbtree_node *mini = rbtree_mini(inst, inst->root);
        rbtree_node *cur = rbtree_delete(inst, mini);
        
        if (cur) {
            if (cur->key) kvs_free(cur->key);
            if (cur->value) kvs_free(cur->value);
            kvs_free(cur);
        }
    }

    // 释放 nil 节点
    if (inst->nil) {
        kvs_free(inst->nil);
        inst->nil = NULL;
    }
}

int kvs_rbtree_set(kvs_rbtree_t *inst, char *key, char *value, long long expire_ms) {
    if (!inst || !key || !value) return -1;

    rbtree_node *node = (rbtree_node*)kvs_malloc(sizeof(rbtree_node));
    if (!node) return -2;
    node->key = kvs_malloc(strlen(key) + 1);
    if (!node->key) { kvs_free(node); return -2; }
    strcpy(node->key, key);
    node->value = kvs_malloc(strlen(value) + 1);
    if (!node->value) { kvs_free(node->key); kvs_free(node); return -2; }
    strcpy(node->value, value);
    node->expire_ms = expire_ms;   // 设置过期时间

    // 查找是否已存在，若存在则替换（保持红黑树性质需先删除再插入）
    rbtree_node *exist = rbtree_search(inst, key);

    if (exist != inst->nil) {
        // 删除原有节点
        rbtree_node *cur = rbtree_delete(inst, exist);
        if (cur) {
            kvs_free(cur->key);
            kvs_free(cur->value);
            kvs_free(cur);
        }
    }
    rbtree_insert(inst, node);
    return 0;
}


char* kvs_rbtree_get(kvs_rbtree_t *inst, char *key) {
    if (!inst || !key) return NULL;
    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return NULL;

    long long now = kvs_current_time_ms();
    if (node->expire_ms != -1 && node->expire_ms <= now) {
        // 惰性删除
        kvs_rbtree_del(inst, key);
        return NULL;
    }
    return node->value;
}



int kvs_rbtree_del(kvs_rbtree_t *inst, char *key) {
    if (!inst || !key) return -1;
    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return 1;   // 键不存在

    rbtree_node *cur = rbtree_delete(inst, node);
    if (cur) {
        kvs_free(cur->key);
        kvs_free(cur->value);
        kvs_free(cur);
    }

    return 0;
}


int kvs_rbtree_mod(kvs_rbtree_t *inst, char *key, char *value) {
    if (!inst || !key || !value) return -1;
    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return 1;  // ???
    kvs_free(node->value);


    node->value = kvs_malloc(strlen(value) + 1);
    if (!node->value) return -2;
    strcpy(node->value, value);
    return 0;
}

int kvs_rbtree_exist(kvs_rbtree_t *inst, char *key) {
    if (!inst || !key) return -1;

    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return 1;

    long long now = kvs_current_time_ms();
    if (node->expire_ms != -1 && node->expire_ms <= now) {
		kvs_rbtree_del(inst, key);
        return 1;
    }

    return 0;
}


int kvs_rbtree_expire(kvs_rbtree_t *inst, char *key, long long expire_ms) {
    if (!inst || !key) return -1;
    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return 1;   // 键不存在
    if (expire_ms == 0) {
        return kvs_rbtree_del(inst, key);
    }
    node->expire_ms = expire_ms;
    return 0;
}

long long kvs_rbtree_ttl(kvs_rbtree_t *inst, char *key) {
    if (!inst || !key) return -2;   // error
    rbtree_node *node = rbtree_search(inst, key);
    if (node == inst->nil) return -2;   // 不存在
    if (node->expire_ms == -1) return -1;   // 永不过期
    long long now = kvs_current_time_ms();
    long long ttl = node->expire_ms - now;
    return (ttl > 0) ? ttl : 0;
}

// 辅助函数：递归遍历红黑树，收集过期的节点指针
static void rbtree_collect_expired(rbtree_node *node, rbtree_node *nil, long long now,
                                   rbtree_node **expired, int *count, int max) {
    if (node == nil) return;
    rbtree_collect_expired(node->left, nil, now, expired, count, max);
    if (node->expire_ms != -1 && node->expire_ms <= now) {
        if (*count < max) {
            expired[(*count)++] = node;
        }
    }
    rbtree_collect_expired(node->right, nil, now, expired, count, max);
}

void kvs_rbtree_scan_expired(kvs_rbtree_t *inst) {
    if (!inst) return;
    long long now = kvs_current_time_ms();
    #define MAX_EXPIRED 10240   // 一次扫描最多处理的过期键数量
    rbtree_node *expired_nodes[MAX_EXPIRED];
    int count = 0;
    
    rbtree_collect_expired(inst->root, inst->nil, now, expired_nodes, &count, MAX_EXPIRED);
    
    for (int i = 0; i < count; i++) {
		rbtree_node *cur = rbtree_delete(inst, expired_nodes[i]);
		if (cur) {
			kvs_free(cur->key);
			kvs_free(cur->value);
			kvs_free(cur);
		}
	}
}