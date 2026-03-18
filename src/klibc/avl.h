/*
 * avl.h - Generic intrusive AVL tree
 *
 * Embed avl_node_t in any struct, create an avl_tree_t with a comparator,
 * and use the provided O(log N) operations. All traversal is non-recursive.
 *
 * Author: u/ApparentlyPlus
 */

#pragma once

#include <stddef.h>

typedef struct avl_node {
    struct avl_node *left, *right, *parent;
    int height;
} avl_node_t;

/* Obtain pointer to enclosing struct from embedded avl_node_t pointer. */
#define AVL_ENTRY(ptr, type, member) \
    ((type*)((char*)(ptr) - offsetof(type, member)))

/*
 * Comparator: negative if a < b, positive if a > b, zero if equal.
 * Both arguments are avl_node_t* embedded in the type being sorted.
 */
typedef int (*avl_cmp_fn)(const avl_node_t* a, const avl_node_t* b);

typedef struct {
    avl_node_t *root;
    avl_cmp_fn  cmp;
} avl_tree_t;

/* Initialize an empty tree with a comparator. */
void avl_init(avl_tree_t* tree, avl_cmp_fn cmp);

/* Insert node into tree. node must not already be in any tree. */
void avl_insert(avl_tree_t* tree, avl_node_t* node);

/* Remove node from tree. node must be in this tree. */
void avl_remove(avl_tree_t* tree, avl_node_t* node);

/* Exact match: returns node where cmp(key,node)==0, or NULL. */
avl_node_t* avl_find(avl_tree_t* tree, avl_node_t* key);

/* Floor: largest node n where cmp(key,n) >= 0. Returns NULL if all nodes > key. */
avl_node_t* avl_floor(avl_tree_t* tree, avl_node_t* key);

/* Ceil: smallest node n where cmp(key,n) <= 0. Returns NULL if all nodes < key. */
avl_node_t* avl_ceil(avl_tree_t* tree, avl_node_t* key);

/* Minimum (leftmost) node, or NULL if empty. */
avl_node_t* avl_min(avl_tree_t* tree);

/* In-order successor of node, or NULL if node is the maximum. */
avl_node_t* avl_next(avl_node_t* node);

/* In-order predecessor of node, or NULL if node is the minimum. */
avl_node_t* avl_prev(avl_node_t* node);
