// heap.cpp — Binary min-heap implementation with back-pointer tracking.

#include "heap.h"
#include <utility>

// Sift up: move an element up until the min-heap property holds.
// At every step, the shifted element's back-pointer is updated.
void heap_up(HeapItem *a, size_t pos) {
    HeapItem t = a[pos];
    while (pos > 0) {
        size_t p = heap_parent(pos);
        if (a[p].val <= t.val) {
            break;
        }
        a[pos] = a[p];
        *a[pos].ref = pos;
        pos = p;
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

// Sift down: move an element down until the min-heap property holds.
void heap_down(HeapItem *a, size_t pos, size_t len) {
    HeapItem t = a[pos];
    while (true) {
        size_t l = heap_left(pos);
        size_t r = heap_right(pos);
        size_t min_idx = pos;
        uint64_t min_val = t.val;

        if (l < len && a[l].val < min_val) {
            min_idx = l;
            min_val = a[l].val;
        }
        if (r < len && a[r].val < min_val) {
            min_idx = r;
            min_val = a[r].val;
        }
        if (min_idx == pos) {
            break;
        }
        a[pos] = a[min_idx];
        *a[pos].ref = pos;
        pos = min_idx;
    }
    a[pos] = t;
    *a[pos].ref = pos;
}

// Sift up or down depending on whether val decreased or increased.
void heap_update(HeapItem *a, size_t pos, size_t len) {
    if (pos > 0 && a[heap_parent(pos)].val > a[pos].val) {
        heap_up(a, pos);
    } else {
        heap_down(a, pos, len);
    }
}

// Insert a new item into the heap at the end and sift up.
void heap_insert(std::vector<HeapItem> &heap, uint64_t val, size_t *ref) {
    size_t pos = heap.size();
    HeapItem item;
    item.val = val;
    item.ref = ref;
    heap.push_back(item);
    *ref = pos;
    heap_up(heap.data(), pos);
}

// Delete an arbitrary item at `pos` from the heap in O(log n).
// The caller's index variable is marked invalid ((size_t)-1).
void heap_delete(std::vector<HeapItem> &heap, size_t pos) {
    if (pos >= heap.size()) {
        return;
    }
    size_t len = heap.size();
    *heap[pos].ref = (size_t)-1; // Invalidate caller's index
    if (pos == len - 1) {
        heap.pop_back();
        return;
    }
    heap[pos] = heap.back();
    *heap[pos].ref = pos;
    heap.pop_back();
    heap_update(heap.data(), pos, heap.size());
}

// Update the expiration timestamp of an item at `pos` and restore heap order in O(log n).
void heap_update_val(std::vector<HeapItem> &heap, size_t pos, uint64_t new_val) {
    if (pos >= heap.size()) {
        return;
    }
    heap[pos].val = new_val;
    heap_update(heap.data(), pos, heap.size());
}

// Pop the minimum element (index 0) from the heap.
bool heap_pop_min(std::vector<HeapItem> &heap, HeapItem *out) {
    if (heap.empty()) {
        return false;
    }
    if (out) {
        *out = heap[0];
    }
    heap_delete(heap, 0);
    return true;
}
