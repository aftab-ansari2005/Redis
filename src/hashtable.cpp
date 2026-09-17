// hashtable.cpp — Implementation of chaining hashtable with incremental rehashing.
//
// Two layers:
//   HTab — single fixed-size table (array of singly-linked bucket chains)
//   HMap — two-table wrapper that handles progressive migration
//
// INCREMENTAL REHASHING STRATEGY:
//   When the load factor exceeds the threshold (entries >= buckets, i.e. load
//   factor >= 1.0), we allocate a new table with 2× the capacity. On every
//   subsequent operation (lookup, insert, delete), we migrate ONE bucket from
//   the old table to the new table.
//
//   This spreads the O(N) rehash cost across the next N operations, giving
//   amortized O(1) per operation with zero latency spikes.

#include "hashtable.h"
#include <cassert>
#include <cstdlib>
#include <cstring>

// Initial capacity for a brand-new HMap (first insert).
static const size_t k_init_cap = 4;

// ==========================================================================
// HTab — single fixed-size chaining hash table
// ==========================================================================

// Initialize a table with `n` bucket slots. `n` must be a power of 2.
// All bucket heads are set to NULL (empty chains).
void h_init(HTab *htab, size_t n) {
    assert(n > 0 && ((n - 1) & n) == 0);  // must be power of 2
    htab->tab = (HNode **)calloc(n, sizeof(HNode *));
    htab->mask = n - 1;
    htab->size = 0;
}

// Insert a node at the HEAD of its bucket's chain.
//
// Why prepend (not append)?
//   - O(1) — no need to traverse the chain.
//   - Temporal locality: recently inserted keys are checked first during lookup,
//     which is often beneficial (think "SET then GET" patterns).
//
// DOES NOT CHECK FOR DUPLICATES. The caller (HMap layer or application) must
// handle upsert logic (lookup + delete old + insert new) if needed.
void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;   // bucket index
    HNode *next = htab->tab[pos];            // current head of chain
    node->next = next;                       // new node points to old head
    htab->tab[pos] = node;                   // new node becomes head
    htab->size++;
}

// Look up a node in the table.
//
// Walk the chain at bucket (key->hcode & mask). For each node, first compare
// hcode (fast integer compare, rejects most non-matches without touching the
// actual key data), then use the caller's comparator for a full key comparison.
//
// Returns the matching node, or NULL.
HNode *h_lookup(HTab *htab, HNode *key, HNodeCmp cmp) {
    if (!htab->tab) {
        return NULL;
    }

    size_t pos = key->hcode & htab->mask;
    HNode *cur = htab->tab[pos];

    while (cur) {
        if (cur->hcode == key->hcode && cmp(cur, key)) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

// Detach (unlink) a node matching `key` from the table.
//
// Uses a pointer-to-pointer technique to avoid special-casing the head:
//   `from` always points to either &tab[pos] (the bucket head pointer)
//   or &(prev->next). In both cases, *from is the pointer we need to update
//   to skip over the detached node.
//
// Returns the detached node (caller must free it), or NULL if not found.
HNode *h_detach(HTab *htab, HNode *key, HNodeCmp cmp) {
    if (!htab->tab) {
        return NULL;
    }

    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];   // pointer to the pointer we'll update

    while (*from) {
        if ((*from)->hcode == key->hcode && cmp(*from, key)) {
            HNode *node = *from;
            *from = node->next;       // unlink: skip over this node
            node->next = NULL;
            htab->size--;
            return node;
        }
        from = &(*from)->next;
    }
    return NULL;
}

// Free the bucket array. Does NOT free any nodes — the caller owns them.
void h_destroy(HTab *htab) {
    free(htab->tab);
    htab->tab = NULL;
    htab->mask = 0;
    htab->size = 0;
}

// ==========================================================================
// HMap — two-table progressive rehashing wrapper
// ==========================================================================

// --------------------------------------------------------------------------
// hm_help_resizing — migrate ONE bucket from ht2 (old) to ht1 (new).
//
// Called from every lookup/insert/delete to amortize the rehash cost.
// Each call does a bounded amount of work: moving all nodes from one bucket
// (one linked list, typically short) from the old table into the new table.
//
// When all buckets in ht2 have been migrated (ht2.size == 0), the old
// table is destroyed and ht2 is set to inactive.
//
// Convention:
//   ht1 = new (bigger) table — inserts go here
//   ht2 = old (smaller) table — being drained
// --------------------------------------------------------------------------
static void hm_help_resizing(HMap *hmap) {
    if (!hmap->ht2.tab) {
        return;   // not currently resizing
    }

    // Move all nodes from the current bucket in ht2 (old table)
    size_t pos = hmap->resizing_pos;
    HNode **from = &hmap->ht2.tab[pos];
    while (*from) {
        HNode *node = *from;
        *from = node->next;           // detach from ht2's chain
        hmap->ht2.size--;

        // Re-insert into ht1 (new table) using the cached hcode
        h_insert(&hmap->ht1, node);
    }

    // Advance to the next bucket
    hmap->resizing_pos++;

    // Check if migration is complete
    if (hmap->ht2.size == 0) {
        // All nodes have been moved to ht1.
        // Destroy the old (now empty) ht2.
        h_destroy(&hmap->ht2);
        hmap->resizing_pos = 0;
    }
}

// --------------------------------------------------------------------------
// hm_start_resizing — begin incremental migration to a 2× larger table.
//
// Allocates a new table (ht2) with double the capacity of ht1, and sets
// resizing_pos to 0. Subsequent operations will call hm_help_resizing()
// to progressively move buckets.
// --------------------------------------------------------------------------
static void hm_start_resizing(HMap *hmap) {
    assert(!hmap->ht2.tab);   // must not already be resizing

    // Save the old table into ht2, then create a new bigger table as ht1.
    //
    // After this swap:
    //   ht1 = new empty big table (2× capacity) — inserts go here
    //   ht2 = old populated small table — being drained by hm_help_resizing
    hmap->ht2 = hmap->ht1;
    h_init(&hmap->ht1, (hmap->ht2.mask + 1) * 2);
    hmap->resizing_pos = 0;
}

// --------------------------------------------------------------------------
// Public HMap API
// --------------------------------------------------------------------------

HNode *hm_lookup(HMap *hmap, HNode *key, HNodeCmp cmp) {
    // Help with migration on every operation
    hm_help_resizing(hmap);

    // Search ht1 (primary / new table during resize)
    HNode *node = h_lookup(&hmap->ht1, key, cmp);
    if (!node) {
        // Fall back to ht2 (old table, if resizing)
        node = h_lookup(&hmap->ht2, key, cmp);
    }
    return node;
}

void hm_insert(HMap *hmap, HNode *node) {
    // First insert? Initialize ht1.
    if (!hmap->ht1.tab) {
        h_init(&hmap->ht1, k_init_cap);
    }

    // Always insert into ht1 (during resize, ht1 is the new bigger table).
    h_insert(&hmap->ht1, node);

    // Check if we need to trigger a resize.
    // Condition: load factor >= 1.0 AND not already resizing.
    //
    // Load factor = total entries / ht1 capacity
    // When entries >= capacity (mask + 1), average chain length >= 1.0.
    // This is a good balance: chains stay short but we don't resize too eagerly.
    if (!hmap->ht2.tab) {
        size_t load = hmap->ht1.size / (hmap->ht1.mask + 1);
        if (load >= 1) {
            hm_start_resizing(hmap);
        }
    }

    // Help with migration
    hm_help_resizing(hmap);
}

HNode *hm_delete(HMap *hmap, HNode *key, HNodeCmp cmp) {
    // Help with migration
    hm_help_resizing(hmap);

    // Try ht1 first (primary / new table)
    HNode *node = h_detach(&hmap->ht1, key, cmp);
    if (!node) {
        // Try ht2 (old table, if resizing)
        node = h_detach(&hmap->ht2, key, cmp);
    }
    return node;
}

size_t hm_size(HMap *hmap) {
    return hmap->ht1.size + hmap->ht2.size;
}

void hm_destroy(HMap *hmap) {
    h_destroy(&hmap->ht1);
    h_destroy(&hmap->ht2);
}
