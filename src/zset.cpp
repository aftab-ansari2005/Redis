#include "zset.h"
#include <cstring>
#include <algorithm>

// Helper to recover ZNode from HNode
static inline ZNode *hnode_znode(HNode *node) {
    return container_of(node, ZNode, hmap);
}

// Helper to recover ZNode from AVLNode
static inline ZNode *avlnode_znode(AVLNode *node) {
    return container_of(node, ZNode, tree);
}

// HNode comparator
static bool znode_eq(HNode *lhs, HNode *rhs) {
    ZNode *le = hnode_znode(lhs);
    ZNode *re = hnode_znode(rhs);
    return le->name == re->name;
}

// Hashing function (FNV)
static uint64_t str_hash(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= (unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Tree comparison: returns true if (score1, name1) < (score2, name2)
static bool znode_less(AVLNode *lhs, double score, const char *name, size_t len) {
    ZNode *zl = avlnode_znode(lhs);
    if (zl->score != score) {
        return zl->score < score;
    }
    int rv = memcmp(zl->name.data(), name, std::min(zl->name.size(), len));
    if (rv != 0) {
        return rv < 0;
    }
    return zl->name.size() < len;
}

static bool znode_less(AVLNode *lhs, AVLNode *rhs) {
    ZNode *zr = avlnode_znode(rhs);
    return znode_less(lhs, zr->score, zr->name.data(), zr->name.size());
}

void zset_init(ZSet *zset) {
    zset->tree = NULL;
    zset->hmap.ht1.tab = NULL;
    zset->hmap.ht2.tab = NULL;
}

// Tree insert helper
static void tree_add(ZSet *zset, ZNode *node) {
    AVLNode *cur = zset->tree;
    if (!cur) {
        node->tree.left = node->tree.right = node->tree.parent = NULL;
        node->tree.height = 1;
        node->tree.cnt = 1;
        zset->tree = &node->tree;
        return;
    }

    while (true) {
        if (znode_less(&node->tree, cur)) {
            if (!cur->left) {
                node->tree.left = node->tree.right = NULL;
                node->tree.parent = cur;
                node->tree.height = 1;
                node->tree.cnt = 1;
                cur->left = &node->tree;
                break;
            }
            cur = cur->left;
        } else {
            if (!cur->right) {
                node->tree.left = node->tree.right = NULL;
                node->tree.parent = cur;
                node->tree.height = 1;
                node->tree.cnt = 1;
                cur->right = &node->tree;
                break;
            }
            cur = cur->right;
        }
    }
    zset->tree = avl_fix(&node->tree);
}

ZNode *zset_lookup(ZSet *zset, const char *name, size_t len) {
    ZNode key;
    key.name.assign(name, len);
    key.hmap.hcode = str_hash(name, len);
    HNode *found = hm_lookup(&zset->hmap, &key.hmap, &znode_eq);
    return found ? hnode_znode(found) : NULL;
}

bool zset_add(ZSet *zset, const char *name, size_t len, double score) {
    ZNode *node = zset_lookup(zset, name, len);
    if (node) {
        if (node->score == score) {
            return false; // No change
        }
        // Update score: detach from tree, update, reinsert
        zset->tree = avl_del(&node->tree);
        node->score = score;
        tree_add(zset, node);
        return false;
    }

    // New node
    node = new ZNode();
    node->name.assign(name, len);
    node->score = score;
    node->hmap.hcode = str_hash(name, len);
    hm_insert(&zset->hmap, &node->hmap);
    tree_add(zset, node);
    return true;
}

ZNode *zset_pop(ZSet *zset, const char *name, size_t len) {
    ZNode key;
    key.name.assign(name, len);
    key.hmap.hcode = str_hash(name, len);
    HNode *found = hm_delete(&zset->hmap, &key.hmap, &znode_eq);
    if (!found) {
        return NULL;
    }
    ZNode *node = hnode_znode(found);
    zset->tree = avl_del(&node->tree);
    return node;
}

int64_t zset_rank(ZSet *zset, const char *name, size_t len) {
    ZNode *node = zset_lookup(zset, name, len);
    if (!node) {
        return -1;
    }
    
    // Find rank by walking up the tree
    AVLNode *cur = &node->tree;
    int64_t rank = avl_cnt(cur->left);
    while (cur->parent) {
        if (cur == cur->parent->right) {
            rank += 1 + avl_cnt(cur->parent->left);
        }
        cur = cur->parent;
    }
    return rank;
}

void zset_query(
    ZSet *zset, double score, const char *name, size_t len,
    int64_t offset, int64_t limit,
    std::vector<std::pair<std::string, double>> &out
) {
    AVLNode *cur = zset->tree;
    AVLNode *found = NULL;
    
    // Find first node >= (score, name)
    while (cur) {
        if (znode_less(cur, score, name, len)) {
            // cur < target, need to go right
            cur = cur->right;
        } else {
            // cur >= target, this is a candidate, but there might be a smaller one on the left
            found = cur;
            cur = cur->left;
        }
    }
    
    if (!found) {
        return; // No such element
    }
    
    if (offset != 0) {
        found = avl_offset(found, offset);
    }
    
    while (found && limit > 0) {
        ZNode *zn = avlnode_znode(found);
        out.push_back({zn->name, zn->score});
        found = avl_offset(found, 1);
        limit--;
    }
}

void zset_destroy(ZSet *zset) {
    // Collect all nodes to delete them
    std::vector<ZNode *> nodes;
    if (zset->tree) {
        AVLNode *cur = zset->tree;
        while (cur->left) cur = cur->left;
        while (cur) {
            nodes.push_back(avlnode_znode(cur));
            cur = avl_offset(cur, 1);
        }
    }
    
    hm_destroy(&zset->hmap);
    for (ZNode *n : nodes) {
        delete n;
    }
    zset->tree = NULL;
}
