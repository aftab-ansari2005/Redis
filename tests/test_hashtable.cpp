// test_hashtable.cpp — Isolated unit tests for HTab/HMap (no networking).
//
// Tests the chaining hashtable and incremental rehashing in isolation,
// verifying correctness during and after migration, under insert/delete load.
//
// Build:  g++ -Wall -Wextra -std=c++14 -o test_hashtable.exe tests/test_hashtable.cpp src/hashtable.cpp
// Run:    ./test_hashtable.exe

#include "../src/hashtable.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <algorithm>

// --------------------------------------------------------------------------
// Test Entry — the application-level struct containing an intrusive HNode.
// Same pattern as server.cpp's Entry, but standalone for testing.
// --------------------------------------------------------------------------
struct TestEntry {
    HNode       node;
    std::string key;
    int         val;      // numeric value for easy verification
};

// FNV-1a hash (same as server.cpp)
static uint64_t str_hash(const std::string &s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

// Equality comparator: recover TestEntry via container_of, compare keys.
static bool test_eq(HNode *lhs, HNode *rhs) {
    TestEntry *le = container_of(lhs, TestEntry, node);
    TestEntry *re = container_of(rhs, TestEntry, node);
    return le->key == re->key;
}

// Helper: create a TestEntry, compute its hash, return it.
static TestEntry *make_entry(const std::string &key, int val) {
    TestEntry *e = new TestEntry();
    e->key = key;
    e->val = val;
    e->node.hcode = str_hash(key);
    e->node.next = NULL;
    return e;
}

// Helper: look up a key in the HMap, return the TestEntry* or NULL.
static TestEntry *lookup(HMap *hmap, const std::string &key) {
    TestEntry tmp;
    tmp.key = key;
    tmp.node.hcode = str_hash(key);
    HNode *node = hm_lookup(hmap, &tmp.node, &test_eq);
    if (!node) return NULL;
    return container_of(node, TestEntry, node);
}

// Helper: delete a key from the HMap, free the entry, return true if found.
static bool del(HMap *hmap, const std::string &key) {
    TestEntry tmp;
    tmp.key = key;
    tmp.node.hcode = str_hash(key);
    HNode *node = hm_delete(hmap, &tmp.node, &test_eq);
    if (!node) return false;
    TestEntry *ent = container_of(node, TestEntry, node);
    delete ent;
    return true;
}

// ==========================================================================
// Test cases
// ==========================================================================

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("[FAIL] %s (line %d): %s\n", __func__, __LINE__, msg); \
        g_fail++; \
        return; \
    } \
} while(0)

#define PASS() do { \
    printf("[PASS] %s\n", __func__); \
    g_pass++; \
} while(0)

// --------------------------------------------------------------------------
// Test 1: Basic insert, lookup, delete on an empty HMap
// --------------------------------------------------------------------------
static void test_basic_ops() {
    HMap hmap = {};

    // Lookup on empty map
    CHECK(lookup(&hmap, "foo") == NULL, "empty map should return NULL");

    // Insert one entry
    TestEntry *e1 = make_entry("foo", 42);
    hm_insert(&hmap, &e1->node);
    CHECK(hm_size(&hmap) == 1, "size should be 1 after insert");

    // Lookup the entry we just inserted
    TestEntry *found = lookup(&hmap, "foo");
    CHECK(found != NULL, "should find 'foo'");
    CHECK(found->val == 42, "value should be 42");

    // Lookup a non-existent key
    CHECK(lookup(&hmap, "bar") == NULL, "'bar' should not exist");

    // Delete the entry
    CHECK(del(&hmap, "foo") == true, "delete 'foo' should succeed");
    CHECK(hm_size(&hmap) == 0, "size should be 0 after delete");
    CHECK(lookup(&hmap, "foo") == NULL, "'foo' should be gone after delete");

    // Delete non-existent
    CHECK(del(&hmap, "foo") == false, "double-delete should return false");

    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 2: Multiple entries, upsert pattern
// --------------------------------------------------------------------------
static void test_multiple_entries() {
    HMap hmap = {};

    // Insert 10 entries
    for (int i = 0; i < 10; i++) {
        std::string key = "key" + std::to_string(i);
        TestEntry *e = make_entry(key, i * 100);
        hm_insert(&hmap, &e->node);
    }
    CHECK(hm_size(&hmap) == 10, "size should be 10");

    // Verify all are found with correct values
    for (int i = 0; i < 10; i++) {
        std::string key = "key" + std::to_string(i);
        TestEntry *found = lookup(&hmap, key);
        CHECK(found != NULL, "should find key");
        CHECK(found->val == i * 100, "value should match");
    }

    // Delete even-numbered keys
    for (int i = 0; i < 10; i += 2) {
        std::string key = "key" + std::to_string(i);
        CHECK(del(&hmap, key) == true, "delete should succeed");
    }
    CHECK(hm_size(&hmap) == 5, "size should be 5 after deleting evens");

    // Verify odd keys still found, even keys gone
    for (int i = 0; i < 10; i++) {
        std::string key = "key" + std::to_string(i);
        TestEntry *found = lookup(&hmap, key);
        if (i % 2 == 0) {
            CHECK(found == NULL, "even key should be gone");
        } else {
            CHECK(found != NULL, "odd key should still exist");
            CHECK(found->val == i * 100, "odd key value should match");
        }
    }

    // Clean up remaining entries
    for (int i = 1; i < 10; i += 2) {
        std::string key = "key" + std::to_string(i);
        del(&hmap, key);
    }
    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 3: Trigger resize, verify all keys found DURING migration
//
// This is the critical test: we insert enough entries to exceed the load
// factor threshold (entries >= buckets), triggering incremental rehashing.
// While both tables are active, we verify that every key is still findable.
// --------------------------------------------------------------------------
static void test_resize_during_migration() {
    HMap hmap = {};

    // Insert enough entries to trigger at least one resize.
    // Initial capacity is 4 (k_init_cap in hashtable.cpp).
    // Load factor threshold is 1.0, so resize triggers at 4 entries.
    // After resize: cap=8. Resize again at 8, etc.
    //
    // Insert 20 entries to trigger multiple resizes.
    const int N = 20;
    std::vector<TestEntry *> entries;
    for (int i = 0; i < N; i++) {
        std::string key = "resize_key_" + std::to_string(i);
        TestEntry *e = make_entry(key, i);
        hm_insert(&hmap, &e->node);
        entries.push_back(e);

        // After every insert, verify ALL previously inserted keys are findable.
        // This tests correctness during active migration.
        for (int j = 0; j <= i; j++) {
            std::string check_key = "resize_key_" + std::to_string(j);
            TestEntry *found = lookup(&hmap, check_key);
            if (!found) {
                printf("[FAIL] %s: key '%s' not found after inserting %d entries "
                       "(ht1.size=%u, ht2.size=%u)\n",
                       __func__, check_key.c_str(), i + 1,
                       (unsigned)hmap.ht1.size, (unsigned)hmap.ht2.size);
                g_fail++;
                // Clean up
                for (auto *e2 : entries) delete e2;
                hm_destroy(&hmap);
                return;
            }
            if (found->val != j) {
                printf("[FAIL] %s: key '%s' has wrong value %d (expected %d)\n",
                       __func__, check_key.c_str(), found->val, j);
                g_fail++;
                for (auto *e2 : entries) delete e2;
                hm_destroy(&hmap);
                return;
            }
        }
    }
    CHECK(hm_size(&hmap) == (size_t)N, "final size should match N");

    // Clean up
    for (int i = 0; i < N; i++) {
        std::string key = "resize_key_" + std::to_string(i);
        del(&hmap, key);
    }
    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 4: Post-resize correctness — force migration to complete, verify
// all keys are in a single table.
// --------------------------------------------------------------------------
static void test_post_resize() {
    HMap hmap = {};

    const int N = 50;
    for (int i = 0; i < N; i++) {
        std::string key = "post_" + std::to_string(i);
        TestEntry *e = make_entry(key, i);
        hm_insert(&hmap, &e->node);
    }

    // Do many lookups to force migration to complete.
    // Each lookup calls hm_help_resizing, moving one bucket.
    // After enough lookups, ht2 should be empty.
    for (int round = 0; round < N * 10; round++) {
        std::string key = "post_" + std::to_string(round % N);
        TestEntry *found = lookup(&hmap, key);
        CHECK(found != NULL, "key should be found during drain");
    }

    // After all that draining, ht2 should be inactive
    CHECK(hmap.ht2.tab == NULL, "ht2 should be NULL after migration completes");

    // Verify all keys still correct in the single remaining table
    for (int i = 0; i < N; i++) {
        std::string key = "post_" + std::to_string(i);
        TestEntry *found = lookup(&hmap, key);
        CHECK(found != NULL, "key should exist post-resize");
        CHECK(found->val == i, "value should match post-resize");
    }

    // Clean up
    for (int i = 0; i < N; i++) {
        std::string key = "post_" + std::to_string(i);
        del(&hmap, key);
    }
    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 5: Delete during resize — verify deleted keys are gone from BOTH
// tables, and non-deleted keys survive.
// --------------------------------------------------------------------------
static void test_delete_during_resize() {
    HMap hmap = {};

    const int N = 30;
    for (int i = 0; i < N; i++) {
        std::string key = "ddr_" + std::to_string(i);
        TestEntry *e = make_entry(key, i);
        hm_insert(&hmap, &e->node);
    }

    // Delete every 3rd key while resize may be in progress
    for (int i = 0; i < N; i += 3) {
        std::string key = "ddr_" + std::to_string(i);
        CHECK(del(&hmap, key) == true, "delete should succeed");
    }

    // Verify: deleted keys are gone, others remain
    for (int i = 0; i < N; i++) {
        std::string key = "ddr_" + std::to_string(i);
        TestEntry *found = lookup(&hmap, key);
        if (i % 3 == 0) {
            CHECK(found == NULL, "deleted key should be gone");
        } else {
            CHECK(found != NULL, "non-deleted key should exist");
            CHECK(found->val == i, "value should match");
        }
    }

    // Clean up remaining
    for (int i = 0; i < N; i++) {
        if (i % 3 != 0) {
            std::string key = "ddr_" + std::to_string(i);
            del(&hmap, key);
        }
    }
    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 6: Randomized stress test
//
// Insert N random keys, randomly delete ~half, verify the remaining half
// is found and the deleted half is not.
// --------------------------------------------------------------------------
static void test_stress() {
    HMap hmap = {};

    // Simple deterministic pseudo-random for reproducibility
    // (no need for <random>, this is a unit test)
    unsigned seed = 12345;
    auto next_rand = [&seed]() -> unsigned {
        seed = seed * 1103515245 + 12345;
        return (seed >> 16) & 0x7fff;
    };

    const int N = 1000;
    std::set<std::string> inserted;   // track what's in the map
    std::set<std::string> deleted;    // track what we've deleted

    // Phase 1: insert N random keys
    for (int i = 0; i < N; i++) {
        std::string key = "stress_" + std::to_string(next_rand()) + "_" + std::to_string(i);
        TestEntry *e = make_entry(key, i);
        hm_insert(&hmap, &e->node);
        inserted.insert(key);
    }
    CHECK(hm_size(&hmap) == inserted.size(), "size should match inserted count");

    // Phase 2: randomly delete ~half
    std::vector<std::string> keys(inserted.begin(), inserted.end());
    for (const auto &key : keys) {
        if (next_rand() % 2 == 0) {
            CHECK(del(&hmap, key) == true, "delete should succeed");
            inserted.erase(key);
            deleted.insert(key);
        }
    }

    // Phase 3: verify remaining keys are found
    for (const auto &key : inserted) {
        TestEntry *found = lookup(&hmap, key);
        if (!found) {
            printf("[FAIL] %s: surviving key '%s' not found\n", __func__, key.c_str());
            g_fail++;
            // Clean up remaining
            for (const auto &k : inserted) del(&hmap, k);
            hm_destroy(&hmap);
            return;
        }
    }

    // Phase 4: verify deleted keys are NOT found
    for (const auto &key : deleted) {
        TestEntry *found = lookup(&hmap, key);
        if (found) {
            printf("[FAIL] %s: deleted key '%s' still found!\n", __func__, key.c_str());
            g_fail++;
            for (const auto &k : inserted) del(&hmap, k);
            hm_destroy(&hmap);
            return;
        }
    }

    printf("  (stress: %u keys surviving, %u deleted, total ops = %d)\n",
           (unsigned)inserted.size(), (unsigned)deleted.size(), N);

    // Clean up
    for (const auto &key : inserted) {
        del(&hmap, key);
    }
    CHECK(hm_size(&hmap) == 0, "map should be empty after full cleanup");
    hm_destroy(&hmap);
    PASS();
}

// --------------------------------------------------------------------------
// Test 7: container_of correctness — verify the macro actually works
// --------------------------------------------------------------------------
static void test_container_of() {
    TestEntry entry;
    entry.key = "test";
    entry.val = 99;
    entry.node.hcode = 0xdeadbeef;

    HNode *np = &entry.node;
    TestEntry *recovered = container_of(np, TestEntry, node);

    CHECK(recovered == &entry, "container_of should recover original pointer");
    CHECK(recovered->key == "test", "recovered key should match");
    CHECK(recovered->val == 99, "recovered val should match");

    PASS();
}

// ==========================================================================
// Main — run all tests
// ==========================================================================
int main() {
    printf("=== Hashtable Unit Tests (no networking) ===\n\n");

    test_container_of();
    test_basic_ops();
    test_multiple_entries();
    test_resize_during_migration();
    test_post_resize();
    test_delete_during_resize();
    test_stress();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}
