/*
 * avl.c - Generic intrusive AVL tree implementation
 *
 * Author: u/ApparentlyPlus
 */

#include <klibc/avl.h>

void avl_init(avl_tree_t* tree, avl_cmp_fn cmp) {
    tree->root = NULL;
    tree->cmp  = cmp;
}

#pragma region Internal AVL Helpers

static inline int node_h(const avl_node_t* n) {
    return n ? n->height : 0;
}

static inline void upd_height(avl_node_t* n) {
    int l = node_h(n->left), r = node_h(n->right);
    n->height = 1 + (l > r ? l : r);
}

static inline int bal(avl_node_t* n) {
    return n ? node_h(n->left) - node_h(n->right) : 0;
}

/*
 * Redirect the child pointer in parent (or tree->root) that currently
 * points to old_child so it points to new_child instead.
 */
static void relink(avl_tree_t* tree, avl_node_t* parent,
                   avl_node_t* old_child, avl_node_t* new_child) {
    if (!parent)
        tree->root = new_child;
    else if (parent->left == old_child)
        parent->left  = new_child;
    else
        parent->right = new_child;
    if (new_child)
        new_child->parent = parent;
}

static avl_node_t* rot_right(avl_tree_t* tree, avl_node_t* y) {
    avl_node_t* x = y->left;
    avl_node_t* T = x->right;
    relink(tree, y->parent, y, x);
    x->right = y;  y->parent = x;
    y->left  = T;  if (T) T->parent = y;
    upd_height(y);
    upd_height(x);
    return x;
}

static avl_node_t* rot_left(avl_tree_t* tree, avl_node_t* x) {
    avl_node_t* y = x->right;
    avl_node_t* T = y->left;
    relink(tree, x->parent, x, y);
    y->left  = x;  x->parent = y;
    x->right = T;  if (T) T->parent = x;
    upd_height(x);
    upd_height(y);
    return y;
}

/*
 * Walk up from n to the root, recomputing heights and rebalancing.
 */
static void fix_up(avl_tree_t* tree, avl_node_t* n) {
    while (n) {
        upd_height(n);
        int b = bal(n);
        if (b > 1) {
            if (bal(n->left) < 0)
                rot_left(tree, n->left);   /* LR case */
            n = rot_right(tree, n);
        } else if (b < -1) {
            if (bal(n->right) > 0)
                rot_right(tree, n->right); /* RL case */
            n = rot_left(tree, n);
        }
        n = n->parent;
    }
}

static avl_node_t* subtree_min(avl_node_t* n) {
    while (n->left) n = n->left;
    return n;
}

#pragma region Public API

void avl_insert(avl_tree_t* tree, avl_node_t* node) {
    node->left = node->right = node->parent = NULL;
    node->height = 1;

    if (!tree->root) { tree->root = node; return; }

    avl_node_t* cur = tree->root;
    avl_node_t* par = NULL;
    while (cur) {
        par = cur;
        cur = (tree->cmp(node, cur) < 0) ? cur->left : cur->right;
    }
    node->parent = par;
    if (tree->cmp(node, par) < 0) par->left  = node;
    else                           par->right = node;
    fix_up(tree, par);
}

void avl_remove(avl_tree_t* tree, avl_node_t* node) {
    avl_node_t* fix;

    if (!node->left) {
        fix = node->parent;
        relink(tree, node->parent, node, node->right);
    } else if (!node->right) {
        fix = node->parent;
        relink(tree, node->parent, node, node->left);
    } else {
        /* Replace node with its in-order successor */
        avl_node_t* succ = subtree_min(node->right);
        fix = (succ->parent == node) ? succ : succ->parent;

        /* Detach succ from its current position */
        relink(tree, succ->parent, succ, succ->right);

        /* Place succ where node was */
        relink(tree, node->parent, node, succ);
        succ->left  = node->left;
        succ->right = node->right;
        succ->height = node->height;  /* fix_up will correct this */
        if (succ->left)  succ->left->parent  = succ;
        if (succ->right) succ->right->parent = succ;
    }
    fix_up(tree, fix);
}

avl_node_t* avl_find(avl_tree_t* tree, avl_node_t* key) {
    avl_node_t* n = tree->root;
    while (n) {
        int c = tree->cmp(key, n);
        if      (c < 0) n = n->left;
        else if (c > 0) n = n->right;
        else            return n;
    }
    return NULL;
}

avl_node_t* avl_floor(avl_tree_t* tree, avl_node_t* key) {
    avl_node_t* n = tree->root, *res = NULL;
    while (n) {
        int c = tree->cmp(key, n);
        if      (c < 0)            n = n->left;
        else if (c > 0) { res = n; n = n->right; }
        else            return n;
    }
    return res;
}

avl_node_t* avl_ceil(avl_tree_t* tree, avl_node_t* key) {
    avl_node_t* n = tree->root, *res = NULL;
    while (n) {
        int c = tree->cmp(key, n);
        if      (c > 0)            n = n->right;
        else if (c < 0) { res = n; n = n->left; }
        else            return n;
    }
    return res;
}

avl_node_t* avl_min(avl_tree_t* tree) {
    if (!tree->root) return NULL;
    return subtree_min(tree->root);
}

avl_node_t* avl_next(avl_node_t* node) {
    if (node->right) return subtree_min(node->right);
    avl_node_t* p = node->parent;
    while (p && node == p->right) { node = p; p = p->parent; }
    return p;
}

avl_node_t* avl_prev(avl_node_t* node) {
    if (node->left) {
        node = node->left;
        while (node->right) node = node->right;
        return node;
    }
    avl_node_t* p = node->parent;
    while (p && node == p->left) { node = p; p = p->parent; }
    return p;
}
