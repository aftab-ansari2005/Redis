// test_heap.cpp — Comprehensive unit test for binary min-heap and intrusive back-pointers.

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <algorithm>
#include <random>
#include "../src/heap.h"

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: %s\n", __LINE__, msg); \
            exit(1); \
        } \
    } while (0)

struct TestNode {
    int id;
    uint64_t score;
    size_t heap_idx = (size_t)-1;
};

// Verify heap property and back-pointer integrity
static void verify_heap(const std::vector<HeapItem> &heap) {
    for (size_t i = 0; i < heap.size(); i++) {
        CHECK(heap[i].ref != nullptr, "Null ref pointer in heap");
        CHECK(*heap[i].ref == i, "Back-pointer does not match array index");
        size_t l = heap_left(i);
        size_t r = heap_right(i);
        if (l < heap.size()) {
            CHECK(heap[i].val <= heap[l].val, "Min-heap property violated at left child");
        }
        if (r < heap.size()) {
            CHECK(heap[i].val <= heap[r].val, "Min-heap property violated at right child");
        }
    }
}

int main() {
    printf("Running test_heap ...\n");

    std::vector<HeapItem> heap;
    std::vector<TestNode> nodes(10);
    for (int i = 0; i < 10; i++) {
        nodes[i].id = i;
        nodes[i].score = (uint64_t)(100 - i * 7); // 100, 93, 86, 79, 72, 65, 58, 51, 44, 37
    }

    // 1. Test insertion
    printf("Test 1: Insertions and back-pointer tracking...\n");
    for (int i = 0; i < 10; i++) {
        heap_insert(heap, nodes[i].score, &nodes[i].heap_idx);
        verify_heap(heap);
        CHECK(nodes[i].heap_idx < heap.size(), "Node heap_idx out of bounds after insert");
    }

    // Minimum should be nodes[9].score = 37 at index 0
    CHECK(heap[0].val == 37, "Root is not the minimum value");
    CHECK(nodes[9].heap_idx == 0, "Node 9 heap_idx is not 0");

    // 2. Test TTL decrease (sift-up)
    printf("Test 2: TTL decrease (sift-up)...\n");
    // Change node 0 (score 100) to score 10 (new minimum)
    nodes[0].score = 10;
    heap_update_val(heap, nodes[0].heap_idx, 10);
    verify_heap(heap);
    CHECK(heap[0].val == 10, "Node 0 did not become root after decrease");
    CHECK(nodes[0].heap_idx == 0, "Node 0 heap_idx is not 0 after decrease");

    // 3. Test TTL increase (sift-down)
    printf("Test 3: TTL increase (sift-down)...\n");
    // Change current root (node 0, score 10) to score 500
    nodes[0].score = 500;
    heap_update_val(heap, nodes[0].heap_idx, 500);
    verify_heap(heap);
    CHECK(nodes[0].heap_idx != 0, "Node 0 remained at root after large increase");
    CHECK(heap[0].val == 37, "New root is not 37");

    // 4. Test arbitrary deletion from middle
    printf("Test 4: Arbitrary deletion from middle...\n");
    size_t target_idx = nodes[5].heap_idx;
    heap_delete(heap, target_idx);
    verify_heap(heap);
    CHECK(nodes[5].heap_idx == (size_t)-1, "Deleted node back-pointer not invalidated");
    CHECK(heap.size() == 9, "Heap size incorrect after deletion");

    // 5. Test pop-min
    printf("Test 5: Pop-min until empty...\n");
    uint64_t prev = 0;
    while (!heap.empty()) {
        HeapItem out;
        bool ok = heap_pop_min(heap, &out);
        CHECK(ok, "heap_pop_min failed on non-empty heap");
        CHECK(out.val >= prev, "Popped values not monotonic");
        prev = out.val;
        verify_heap(heap);
    }
    CHECK(heap.empty(), "Heap not empty");

    // 6. Randomized stress test
    printf("Test 6: Randomized stress test (1000 items)...\n");
    std::mt19937 rng(42);
    std::vector<TestNode> stress_nodes(1000);
    for (size_t i = 0; i < stress_nodes.size(); i++) {
        stress_nodes[i].id = (int)i;
        stress_nodes[i].score = rng() % 100000;
        heap_insert(heap, stress_nodes[i].score, &stress_nodes[i].heap_idx);
    }
    verify_heap(heap);

    // Random updates and deletes
    for (int step = 0; step < 500; step++) {
        int idx = rng() % stress_nodes.size();
        if (stress_nodes[idx].heap_idx != (size_t)-1) {
            if (rng() % 2 == 0) {
                // Update
                uint64_t new_score = rng() % 100000;
                stress_nodes[idx].score = new_score;
                heap_update_val(heap, stress_nodes[idx].heap_idx, new_score);
            } else {
                // Delete
                heap_delete(heap, stress_nodes[idx].heap_idx);
            }
            verify_heap(heap);
        }
    }

    // Drain remaining
    prev = 0;
    while (!heap.empty()) {
        HeapItem out;
        heap_pop_min(heap, &out);
        CHECK(out.val >= prev, "Randomized drain not sorted");
        prev = out.val;
    }

    printf("[PASS] All heap unit tests passed successfully!\n");
    return 0;
}
