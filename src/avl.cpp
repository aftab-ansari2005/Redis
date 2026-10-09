// avl.cpp — AVL tree implementation: rotations, fixup, delete, rank query.
//
// This file implements the core structural operations for an intrusive AVL
// tree. It does NOT contain comparison logic — the caller is responsible for
// BST-walk and attachment. This module only handles:
//   - Updating height/cnt metadata
//   - Detecting and fixing balance violations (rotations)
//   - Node deletion (detach + rebalance)
//   - Rank-based offset queries
//
// ROTATION RECAP:
//   AVL balance factor = height(left) - height(right)
//   Valid range: {-1, 0, +1}
//
//   bf = +2 → left-heavy:
//     If left child bf >= 0  → LL case → single right rotation
//     If left child bf <  0  → LR case → left-rotate left child, then right-rotate node
//
//   bf = -2 → right-heavy:
//     If right child bf <= 0 → RR case → single left rotation
//     If right child bf >  0 → RL case → right-rotate right child, then left-rotate node

#include "avl.h"
#include <algorithm>  // std::max

// --------------------------------------------------------------------------
// Internal helpers
// --------------------------------------------------------------------------

// Update a node's height and cnt from its children.
// Must be called after any structural change (rotation, child swap, etc.)
static void avl_update(AVLNode *node) {
    node->height = 1 + std::max(avl_height(node->left), avl_height(node->right));
    node->cnt    = 1 + avl_cnt(node->left) + avl_cnt(node->right);
}

// --------------------------------------------------------------------------
// rot_left — single left rotation.
//
//      x                y
//     /  \              /  \
//    T1   y    ->      x   T3
//        /  \         /  \
//       T2  T3      T1  T2
//
// 'x' is the node being rotated down, 'y' is its right child moving up.
// Parent pointers of x, y, and the "transferred" subtree T2 are all updated.
// --------------------------------------------------------------------------
static AVLNode *rot_left(AVLNode *node) {
    AVLNode *rchild = node->right;
    AVLNode *mid    = rchild->left;   // T2: the subtree that changes parent

    // Perform rotation
    rchild->left = node;
    node->right  = mid;

    // Update parent pointers
    //   rchild takes node's parent
    rchild->parent = node->parent;
    node->parent   = rchild;
    if (mid) {
        mid->parent = node;
    }

    // Update height/cnt — order matters: node first (it's now lower)
    avl_update(node);
    avl_update(rchild);

    return rchild;  // new local root
}

// --------------------------------------------------------------------------
// rot_right — single right rotation (mirror of rot_left).
//
//        x              y
//       /  \            /  \
//      y   T3   ->     T1  x
//     /  \                 /  \
//    T1  T2              T2  T3
// --------------------------------------------------------------------------
static AVLNode *rot_right(AVLNode *node) {
    AVLNode *lchild = node->left;
    AVLNode *mid    = lchild->right;  // T2

    // Perform rotation
    lchild->right = node;
    node->left    = mid;

    // Update parent pointers
    lchild->parent = node->parent;
    node->parent   = lchild;
    if (mid) {
        mid->parent = node;
    }

    // Update height/cnt
    avl_update(node);
    avl_update(lchild);

    return lchild;  // new local root
}

// --------------------------------------------------------------------------
// avl_fix_one — check and fix balance at a single node.
//
// Returns the node that should replace 'node' at its position in the tree
// (could be 'node' itself if no rotation was needed, or the new local root
// after rotation).
// --------------------------------------------------------------------------
static AVLNode *avl_fix_one(AVLNode *node) {
    avl_update(node);

    int bf = (int)avl_height(node->left) - (int)avl_height(node->right);

    if (bf == 2) {
        // Left-heavy
        // Check which sub-case:
        //   If left child is right-heavy (bf < 0), it's the LR case.
        //   Otherwise it's the LL case.
        if ((int)avl_height(node->left->left) < (int)avl_height(node->left->right)) {
            // LR case: left-rotate the left child first to reduce to LL
            node->left = rot_left(node->left);
            // Fix parent pointer of the new left child
            node->left->parent = node;
        }
        // LL case (or LR reduced to LL): right-rotate node
        return rot_right(node);
    }

    if (bf == -2) {
        // Right-heavy
        if ((int)avl_height(node->right->right) < (int)avl_height(node->right->left)) {
            // RL case: right-rotate the right child first to reduce to RR
            node->right = rot_right(node->right);
            node->right->parent = node;
        }
        // RR case (or RL reduced to RR): left-rotate node
        return rot_left(node);
    }

    // |bf| <= 1 — already balanced, just update was enough
    return node;
}

// --------------------------------------------------------------------------
// avl_fix — walk up from 'node' to the root, fixing balance at each level.
//
// After inserting a new leaf or deleting a node, the balance property may
// be violated at one or more ancestors. We fix each one and continue up.
//
// Returns the (possibly new) root of the entire tree.
// --------------------------------------------------------------------------
AVLNode *avl_fix(AVLNode *node) {
    while (true) {
        AVLNode *parent = node->parent;
        AVLNode *fixed  = avl_fix_one(node);

        // If a rotation happened, 'fixed' is a different node than 'node'.
        // We need to update the parent's child pointer to point to 'fixed'.
        if (parent) {
            if (parent->left == node) {
                parent->left = fixed;
            } else {
                parent->right = fixed;
            }
            // parent pointer was already set by rot_left/rot_right,
            // but if no rotation happened we still need it correct
            fixed->parent = parent;
        }

        // Move up
        node = fixed;
        if (!node->parent) {
            return node;  // reached the root
        }
        node = node->parent;
    }
}

// --------------------------------------------------------------------------
// avl_del — delete a node from the tree.
//
// The standard BST delete has three cases:
//   1. Leaf (no children) — just unlink from parent
//   2. One child — replace node with its child
//   3. Two children — swap with in-order successor (leftmost node in right
//      subtree), then delete the successor from its original position
//      (which is now case 1 or 2)
//
// After detaching, we rebalance from the lowest affected point up to root.
//
// Returns the new root of the tree (may be NULL if tree is now empty).
// --------------------------------------------------------------------------
AVLNode *avl_del(AVLNode *node) {
    if (node->left && node->right) {
        // Case 3: two children.
        // Find the in-order successor: leftmost node of right subtree.
        AVLNode *successor = node->right;
        while (successor->left) {
            successor = successor->left;
        }

        // We'll "swap" node and successor by transplanting successor's data
        // position into node's position in the tree. But since we're using
        // intrusive nodes (the node IS inside the caller's struct, we can't
        // just swap data), we instead rewire all the pointers.
        //
        // Strategy: detach successor from its current position, then
        // replace 'node' in the tree with 'successor' by copying all of
        // node's tree links onto successor.

        // First, detach successor from its current position.
        // Successor has at most one child (a right child), since it's the
        // leftmost node in a subtree.
        AVLNode *succ_parent = successor->parent;
        AVLNode *succ_right  = successor->right;

        // Unlink successor from its parent
        if (succ_parent->left == successor) {
            succ_parent->left = succ_right;
        } else {
            succ_parent->right = succ_right;
        }
        if (succ_right) {
            succ_right->parent = succ_parent;
        }

        // Now transplant successor into node's position
        successor->left   = node->left;
        successor->right  = node->right;
        successor->parent = node->parent;

        if (successor->left)  successor->left->parent  = successor;
        if (successor->right) successor->right->parent = successor;

        // Update node's parent to point to successor
        if (node->parent) {
            if (node->parent->left == node) {
                node->parent->left = successor;
            } else {
                node->parent->right = successor;
            }
        }

        // The point from which we need to rebalance is the lowest point
        // that was affected. If successor was node's direct right child,
        // that's successor itself. Otherwise it's succ_parent.
        AVLNode *fixup_from;
        if (succ_parent == node) {
            // Successor was the direct right child of node.
            // After transplant, successor->right may have changed (it took
            // node->right, but successor WAS node->right). We need to fix:
            // successor->right should be succ_right (successor's old right child)
            successor->right = succ_right;
            if (succ_right) succ_right->parent = successor;
            fixup_from = successor;
        } else {
            fixup_from = succ_parent;
        }

        // Clear node's pointers (it's now detached)
        node->left = node->right = node->parent = NULL;
        node->height = 0;
        node->cnt = 0;

        return avl_fix(fixup_from);
    }

    // Case 1 or 2: zero or one child.
    AVLNode *child = node->left ? node->left : node->right;
    AVLNode *parent = node->parent;

    // Replace node with its child (or NULL if leaf)
    if (child) {
        child->parent = parent;
    }

    if (parent) {
        if (parent->left == node) {
            parent->left = child;
        } else {
            parent->right = child;
        }
        // Clear node's pointers
        node->left = node->right = node->parent = NULL;
        node->height = 0;
        node->cnt = 0;
        // Rebalance from parent up
        return avl_fix(parent);
    } else {
        // Node was the root
        node->left = node->right = node->parent = NULL;
        node->height = 0;
        node->cnt = 0;
        // child is the new root (may be NULL if tree is now empty)
        if (child) child->parent = NULL;
        return child;
    }
}

// --------------------------------------------------------------------------
// avl_offset — find the node at a relative offset from 'node' in in-order
// traversal, using the cnt (subtree size) augmentation.
//
// Algorithm:
//   We convert the problem to "find the k-th node in a subtree" by computing
//   node's rank within its tree, adding the offset, then walking from root.
//
//   But the book's (and our) approach is more elegant: we walk up and down
//   the tree, tracking how far we still need to go. This avoids needing the
//   root pointer.
//
// WALK-BASED APPROACH:
//   Positive offset: move right (toward larger keys)
//   Negative offset: move left (toward smaller keys)
//
//   At each step, we compute how many nodes in the current subtree are in
//   the direction we want, and either consume them or skip past.
// --------------------------------------------------------------------------
AVLNode *avl_offset(AVLNode *node, int64_t offset) {
    if (!node) return NULL;

    // First, compute the rank of 'node' within the entire tree.
    // rank = number of nodes to the left of 'node' in the tree.
    int64_t pos = (int64_t)avl_cnt(node->left);  // rank within own subtree

    // Walk up to root, accumulating rank
    AVLNode *cur = node;
    while (cur->parent) {
        if (cur == cur->parent->right) {
            // cur is a right child → all of parent's left subtree + parent itself
            // are before cur in sorted order
            pos += 1 + (int64_t)avl_cnt(cur->parent->left);
        }
        cur = cur->parent;
    }
    // 'cur' is now the root, 'pos' is node's global rank (0-indexed)

    // Target rank
    int64_t target = pos + offset;

    // Now find the node at rank 'target' starting from root 'cur'
    if (target < 0 || target >= (int64_t)avl_cnt(cur)) {
        return NULL;  // out of range
    }

    // Walk down from root to find rank 'target'
    while (cur) {
        uint32_t left_cnt = avl_cnt(cur->left);
        if (target < (int64_t)left_cnt) {
            cur = cur->left;
        } else if (target == (int64_t)left_cnt) {
            return cur;  // found it
        } else {
            target -= (int64_t)left_cnt + 1;
            cur = cur->right;
        }
    }

    return NULL;  // shouldn't reach here if tree is consistent
}
