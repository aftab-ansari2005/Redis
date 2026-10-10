// heap.h — Binary min-heap with intrusive back-pointers for O(log n) updates.
//
// DESIGN OVERVIEW:
//   An array-based binary min-heap where each node holds:
//     val — absolute expiration timestamp in microseconds
//     ref — pointer to the caller's struct index variable (e.g. &Entry::heap_idx)
//
// INTRUSIVE INDEX TRACKING:
//   Whenever an element moves in the heap array (during sift-up, sift-down, or swap),
//   the heap writes the element's new array index into `*ref`:
//       *a[pos].ref = pos;
//
//   Why is this critical?
//   1. O(log n) TTL refresh: when EXPIRE is called on a key that already has an expiry,
//      the server doesn't search through the heap array in O(n). It accesses
//      `entry->heap_idx` directly and updates its position in O(log n).
//   2. O(log n) arbitrary removal: when DEL, PERSIST, or SET overwrites a key,
//      we can delete its heap node in O(log n) by swapping with the last element.
//   3. O(1) peek: the earliest-expiring key is always at index 0.

#ifndef HEAP_H
#define HEAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

struct HeapItem {
    uint64_t val = 0;       // Expiry timestamp (microseconds)
    size_t  *ref = nullptr; // Pointer to caller-owned index field (e.g. &Entry::heap_idx)
};

// Heap parent and child index calculations (0-indexed)
inline size_t heap_parent(size_t i) { return (i - 1) / 2; }
inline size_t heap_left(size_t i)   { return 2 * i + 1; }
inline size_t heap_right(size_t i)  { return 2 * i + 2; }

// Low-level array operations
void heap_up(HeapItem *a, size_t pos);
void heap_down(HeapItem *a, size_t pos, size_t len);
void heap_update(HeapItem *a, size_t pos, size_t len);

// High-level operations on std::vector<HeapItem>
void heap_insert(std::vector<HeapItem> &heap, uint64_t val, size_t *ref);
void heap_delete(std::vector<HeapItem> &heap, size_t pos);
void heap_update_val(std::vector<HeapItem> &heap, size_t pos, uint64_t new_val);
bool heap_pop_min(std::vector<HeapItem> &heap, HeapItem *out = nullptr);

#endif // HEAP_H
