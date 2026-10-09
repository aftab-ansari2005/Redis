// zset.h -- Sorted Set (ZSet): hashtable + AVL tree dual-structure.
//
// DESIGN:
//   A ZSet maps (name -> score) with two simultaneous access patterns:
//     1. O(1)     lookup/insert/delete by member name   (via HMap hashtable)
//     2. O(log n) rank/range queries by (score, name)   (via AVL tree)
//
//   This dual-structure design is the same approach real Redis uses (dict +
//   skip list), except we use our AVL tree instead of a skip list.
//
//   Each member is stored in a single ZNode struct that embeds BOTH an HNode
//   (for hashtable membership) and an AVLNode (for tree membership). This
//   intrusive design means one allocation per member, and the node participates
//   in both containers simultaneously.
//
// SORT KEY:
//   The AVL tree is ordered by (score, name) -- the combined key. Score is
//   compared first (numerically); on ties, the member name is compared
//   lexicographically. This ensures every node has a unique, deterministic
//   position in the tree, which is required for ZRANK to return a stable result.
//
//   Why not sort by score alone? Because duplicate scores are common (e.g.
//   multiple players with the same points), and without a tie-breaker:
//     - ZRANK would be non-deterministic
//     - ZRANGE would return arbitrary subsets
//     - ZREM would need to search among score-duplicates to find the right one
//
// SCORE UPDATES (ZADD on existing member):
//   When a member's score changes, we CANNOT mutate the score in-place because
//   the AVL tree is keyed by score. In-place mutation would violate the BST
//   ordering invariant. Instead we: detach from AVL tree, update score,
//   re-insert into AVL tree at the correct new position. The hashtable entry
//   stays in place (it's keyed by name, which doesn't change).

#ifndef ZSET_H
#define ZSET_H

#include "avl.h"
#include "hashtable.h"
#include <string>
#include <vector>
#include <utility>

// --------------------------------------------------------------------------
// ZNode -- a single member in a sorted set.
//
// Embeds both an HNode and an AVLNode so it can live in the hashtable
// and the AVL tree simultaneously without extra allocations.
// --------------------------------------------------------------------------
struct ZNode {
    AVLNode tree;       // AVL tree node (ordered by (score, name))
    HNode   hmap;       // hashtable node (keyed by name)
    double  score;
    // 'name' is stored at the end. We use a std::string for simplicity.
    // (The book uses a flexible array member for zero-copy, but std::string
    // is clearer for learning purposes.)
    std::string name;
};

// --------------------------------------------------------------------------
// ZSet -- the sorted set container.
//
// Contains:
//   tree  -- AVL tree root, ordered by (score, name)
//   hmap  -- hashtable mapping name -> ZNode
// --------------------------------------------------------------------------
struct ZSet {
    AVLNode *tree;      // AVL tree root (NULL if empty)
    HMap     hmap;      // hashtable for name -> ZNode lookup
};

// --------------------------------------------------------------------------
// ZSet operations
// --------------------------------------------------------------------------

// Initialize a ZSet (zeroes everything).
void zset_init(ZSet *zset);

// Add a member with the given score. If the member already exists:
//   - If the score is different: update the score (remove + re-insert in tree)
//   - If the score is the same: no-op
// Returns true if a NEW member was added, false if an existing one was updated.
bool zset_add(ZSet *zset, const char *name, size_t len, double score);

// Look up a member by name. Returns the ZNode, or NULL if not found.
ZNode *zset_lookup(ZSet *zset, const char *name, size_t len);

// Remove a member by name. Returns the detached ZNode (caller must free it),
// or NULL if the member was not found.
ZNode *zset_pop(ZSet *zset, const char *name, size_t len);

// Find the rank (0-indexed position in sorted order) of a member.
// Returns -1 if the member is not found.
// Rank 0 = lowest score, rank (n-1) = highest score.
int64_t zset_rank(ZSet *zset, const char *name, size_t len);

// Query the sorted set by score range + offset + limit.
// This is for the ZQUERY command:
//   - Find the first entry >= (score, name)
//   - Skip 'offset' entries forward
//   - Return up to 'limit' entries
//   - Writes (name, score) pairs into the output vector
//
// 'name' provides the tie-breaking lower bound for entries with the same
// score. Pass "" to start from the first entry at that score.
void zset_query(
    ZSet *zset, double score, const char *name, size_t len,
    int64_t offset, int64_t limit,
    std::vector<std::pair<std::string, double>> &out
);

// Destroy a ZSet and free all its nodes.
void zset_destroy(ZSet *zset);

#endif // ZSET_H
