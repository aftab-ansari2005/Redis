// hashtable.h — Chaining hashtable with incremental (progressive) rehashing.
//
// DESIGN OVERVIEW:
//   Two layers:
//     HTab  — a single fixed-size chaining hash table (array of singly-linked lists)
//     HMap  — wraps two HTabs and manages progressive migration between them
//
//   During a resize, both ht1 (old, shrinking) and ht2 (new, growing) are active.
//   Every lookup/insert/delete moves a small fixed number of buckets from ht1 → ht2,
//   so no single operation ever pays the full cost of rehashing the entire table.
//
// NODE STYLE — INTRUSIVE:
//   HNode is embedded directly inside the caller's struct (e.g. Entry). There is no
//   wrapper or container that copies/owns the data. To go from an HNode* back to the
//   outer struct, use the container_of() macro (same technique as the Linux kernel).
//
//   Why intrusive?
//   1. One allocation per entry, not two (no wrapper node + separate data).
//   2. Better cache locality (node metadata and data are in the same allocation).
//   3. A single struct can participate in MULTIPLE containers simultaneously
//      (e.g. hashtable AND AVL tree — we'll exploit this for sorted sets later).

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <cstddef>
#include <cstdint>

// --------------------------------------------------------------------------
// container_of — recover a pointer to the enclosing struct from a pointer
// to one of its members.
//
// Example:
//   struct Entry { HNode node; std::string key; };
//   HNode *np = ...;
//   Entry *ep = container_of(np, Entry, node);
//
// HOW IT WORKS:
//   (char *)ptr                — address of the member as a raw byte pointer
//   offsetof(type, member)     — byte offset of 'member' within 'type'
//   subtracting gives the address of the enclosing struct
//
// This is standard C/C++ — offsetof is defined by <cstddef>, and the pointer
// arithmetic is well-defined as long as ptr actually points into a 'type'.
// --------------------------------------------------------------------------
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

// --------------------------------------------------------------------------
// HNode — intrusive hash node.
//
// Embedded inside the caller's struct. Contains:
//   next  — singly-linked list pointer (for chaining within a bucket)
//   hcode — cached hash code (avoids recomputing during rehash/lookup)
//
// Why cache the hash code?
// During incremental rehashing we need to re-index nodes into the new table.
// Without cached hcode, we'd need access to the key + hash function to
// recompute it. Caching it in the node makes rehashing self-contained.
// --------------------------------------------------------------------------
struct HNode {
    HNode   *next;      // next node in this bucket's chain (NULL = end)
    uint64_t hcode;     // cached hash code
};

// --------------------------------------------------------------------------
// HTab — a single fixed-size chaining hash table.
//
//   tab   — dynamically allocated array of (HNode *) bucket heads
//   mask  — (capacity - 1), used for fast modulo: bucket = hcode & mask
//           This requires capacity to always be a power of 2.
//   size  — number of entries currently stored
//
// Operations at this level are simple: insert at head, linear scan of chain.
// The HMap layer above handles resizing.
// --------------------------------------------------------------------------
struct HTab {
    HNode **tab;    // array of bucket head pointers
    size_t  mask;   // capacity - 1 (for & masking)
    size_t  size;   // number of entries stored
};

// --------------------------------------------------------------------------
// HMap — two-table wrapper with incremental rehashing.
//
//   ht1           — the "primary" table (or the "old" table during rehash)
//   ht2           — the "new" (larger) table, only active during rehash
//   resizing_pos  — index of the next bucket in ht1 to migrate
//
// Life cycle:
//   Normal:    only ht1 is allocated, ht2.tab == NULL
//   Resizing:  both allocated, resizing_pos advances from 0 to ht1.mask
//   Done:      ht1 is freed, ht2 becomes ht1, ht2 is reset
//
// Every mutating operation (insert/delete) and every lookup calls
// hm_help_resizing() which migrates ONE bucket from ht1 → ht2.
// --------------------------------------------------------------------------
struct HMap {
    HTab   ht1;            // primary (or old during rehash)
    HTab   ht2;            // new table (only during rehash)
    size_t resizing_pos;   // next bucket to migrate
};

// Equality comparator callback type.
// Returns true if two HNodes refer to the same logical key.
// The implementation uses container_of to access the outer struct's key field.
typedef bool (*HNodeCmp)(HNode *, HNode *);

// --------------------------------------------------------------------------
// HTab-level operations (single table, no resizing logic)
// --------------------------------------------------------------------------

// Initialize a table with n buckets (n must be a power of 2, or 0).
void h_init(HTab *htab, size_t n);

// Insert a node into the table. Does NOT check for duplicates.
void h_insert(HTab *htab, HNode *node);

// Look up a node matching key's hcode + comparator. Returns NULL if not found.
HNode *h_lookup(HTab *htab, HNode *key, HNodeCmp cmp);

// Detach (unlink) a node matching key from the table. Returns the detached
// node, or NULL if not found. The caller is responsible for freeing it.
HNode *h_detach(HTab *htab, HNode *key, HNodeCmp cmp);

// Free the bucket array. Does NOT free any nodes (caller owns them).
void h_destroy(HTab *htab);

// --------------------------------------------------------------------------
// HMap-level operations (two-table progressive rehashing)
// --------------------------------------------------------------------------

// Look up a key in the map (searches both tables if resizing).
HNode *hm_lookup(HMap *hmap, HNode *key, HNodeCmp cmp);

// Insert a node. Triggers resize if load factor threshold is exceeded.
void hm_insert(HMap *hmap, HNode *node);

// Delete a node matching key. Returns the detached node (caller frees it),
// or NULL if not found.
HNode *hm_delete(HMap *hmap, HNode *key, HNodeCmp cmp);

// Return the total number of entries across both tables.
size_t hm_size(HMap *hmap);

// Free all internal storage (both tables). Does NOT free nodes.
void hm_destroy(HMap *hmap);

#endif // HASHTABLE_H
