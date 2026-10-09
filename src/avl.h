// avl.h — Intrusive AVL tree with subtree-size augmentation.
//
// DESIGN:
//   This is an intrusive balanced binary tree: the AVLNode struct is embedded
//   inside the caller's own struct (e.g. a ZSet entry), just like HNode is
//   embedded for hash table membership. Use container_of() to recover the
//   outer struct from an AVLNode pointer.
//
//   Each node is augmented with:
//     - height  : tree height rooted at this node (leaf = 1, NULL = 0)
//     - cnt     : subtree size (number of nodes in this subtree, including self)
//
//   The height field maintains the AVL invariant (|balance factor| <= 1).
//   The cnt field enables O(log n) rank/offset queries (find the k-th node
//   in sorted order), which we'll need for ZRANGE in the sorted set.
//
// OWNERSHIP:
//   The tree does NOT own the nodes. The caller allocates and frees them.
//   The tree only manages pointers (left/right/parent) and metadata
//   (height/cnt). avl_del() detaches a node from the tree but does not free
//   it — the caller must free the outer struct.
//
// COMPARISON:
//   This module does NOT store a comparison function. The caller is responsible
//   for finding the correct insertion point (by walking the tree) and passing
//   the correct node to avl_fix() or avl_del(). This keeps the tree code
//   generic — the same implementation works for any key type without templates
//   or function pointers stored in the tree.
//
// WHY AVL AND NOT A SKIP LIST?
//   Real Redis uses a skip list for its sorted set. The book uses an AVL tree
//   because:
//     - It teaches balanced-BST fundamentals (rotations, rebalancing)
//     - AVL has a tighter height bound than red-black (1.44 log n vs 2 log n)
//     - The rank-query augmentation technique generalizes to any BST
//   A skip list would be probabilistically balanced and simpler to implement,
//   but wouldn't teach the rotation mechanics that interviewers love to ask about.

#ifndef AVL_H
#define AVL_H

#include <cstddef>
#include <cstdint>

// --------------------------------------------------------------------------
// AVLNode — intrusive AVL tree node.
//
// Embedded inside the caller's struct. Contains:
//   left    — pointer to left child (NULL if none)
//   right   — pointer to right child (NULL if none)
//   parent  — pointer to parent (NULL for root)
//   height  — height of subtree rooted here (1 for leaf, 0 for NULL)
//   cnt     — number of nodes in subtree rooted here (1 for leaf, 0 for NULL)
//
// Parent pointers are needed for:
//   1. Walking up to rebalance after insert/delete
//   2. The avl_offset() rank query used by sorted-set range commands
// --------------------------------------------------------------------------
struct AVLNode {
    AVLNode *left;
    AVLNode *right;
    AVLNode *parent;
    uint32_t height;    // subtree height (leaf = 1)
    uint32_t cnt;       // subtree size   (leaf = 1)
};

// --------------------------------------------------------------------------
// avl_fix — rebalance the tree starting from the given node upward.
//
// Call this after you've attached a new leaf to the tree (set its
// left = right = NULL, parent = its parent, and linked it as parent's
// left/right child). avl_fix() will:
//   1. Update height and cnt for the node
//   2. Check the balance factor
//   3. Perform the appropriate rotation(s) if |bf| > 1
//   4. Walk up to the root, repeating steps 1–3
//
// Returns the new root of the tree (may change due to rotations).
//
// IMPORTANT: the node passed in must already be linked into the tree
// (parent/child pointers set). avl_fix() does NOT insert — it only
// rebalances. The caller handles the BST walk and attachment.
// --------------------------------------------------------------------------
AVLNode *avl_fix(AVLNode *node);

// --------------------------------------------------------------------------
// avl_del — remove a node from the tree and rebalance.
//
// Detaches 'node' from the tree, handling all three BST-delete cases:
//   - Leaf node: just unlink
//   - One child: replace with child
//   - Two children: swap with in-order successor, then delete from new position
//
// Returns the new root of the tree (may change, or may be NULL if tree is
// now empty). Does NOT free the node — caller owns the memory.
// --------------------------------------------------------------------------
AVLNode *avl_del(AVLNode *node);

// --------------------------------------------------------------------------
// avl_offset — rank query: find the node at a given offset from 'node'
// in in-order (sorted) traversal.
//
//   offset > 0 : move forward (toward larger keys)
//   offset < 0 : move backward (toward smaller keys)
//   offset = 0 : return 'node' itself
//
// Returns NULL if the offset goes past the beginning or end of the tree.
//
// This uses the cnt (subtree size) augmentation for O(log n) performance.
// Without cnt, finding rank k would require O(n) in-order traversal.
//
// EXAMPLE (sorted set ZRANGE by rank):
//   To get elements ranked lo..hi:
//     1. Walk from root to find the lo-th element
//     2. Use avl_offset(lo_node, +1) repeatedly to get lo+1, lo+2, ..., hi
//   Each avl_offset call is O(log n).
// --------------------------------------------------------------------------
AVLNode *avl_offset(AVLNode *node, int64_t offset);

// --------------------------------------------------------------------------
// Utility: get height/cnt safely (handles NULL)
// --------------------------------------------------------------------------
inline uint32_t avl_height(AVLNode *node) {
    return node ? node->height : 0;
}

inline uint32_t avl_cnt(AVLNode *node) {
    return node ? node->cnt : 0;
}

#endif // AVL_H
