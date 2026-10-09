#include "../src/zset.h"
#include <cstdio>
#include <cassert>
#include <string>
#include <vector>

static void test_zset_basic() {
    printf("Test zset basic operations... ");
    ZSet zset;
    zset_init(&zset);
    
    // Add new
    assert(zset_add(&zset, "alice", 5, 10.0));
    assert(zset_add(&zset, "bob", 3, 20.0));
    
    // Update existing
    assert(!zset_add(&zset, "alice", 5, 15.0));
    
    // Lookup
    ZNode *zn = zset_lookup(&zset, "alice", 5);
    assert(zn && zn->score == 15.0);
    
    zn = zset_lookup(&zset, "charlie", 7);
    assert(!zn);
    
    // Rank
    assert(zset_rank(&zset, "alice", 5) == 0); // 15.0 < 20.0
    assert(zset_rank(&zset, "bob", 3) == 1);
    
    // Zrem
    zn = zset_pop(&zset, "alice", 5);
    assert(zn);
    delete zn;
    
    assert(!zset_lookup(&zset, "alice", 5));
    assert(zset_rank(&zset, "bob", 3) == 0);
    
    zset_destroy(&zset);
    printf("PASSED\n");
}

static void test_zset_duplicate_scores() {
    printf("Test zset duplicate scores... ");
    ZSet zset;
    zset_init(&zset);
    
    assert(zset_add(&zset, "charlie", 7, 10.0));
    assert(zset_add(&zset, "alice", 5, 10.0));
    assert(zset_add(&zset, "bob", 3, 10.0));
    
    // Ranks should be lexicographical: alice (0), bob (1), charlie (2)
    assert(zset_rank(&zset, "alice", 5) == 0);
    assert(zset_rank(&zset, "bob", 3) == 1);
    assert(zset_rank(&zset, "charlie", 7) == 2);
    
    zset_destroy(&zset);
    printf("PASSED\n");
}

static void test_zset_query() {
    printf("Test zset query... ");
    ZSet zset;
    zset_init(&zset);
    
    zset_add(&zset, "a", 1, 10.0);
    zset_add(&zset, "b", 1, 20.0);
    zset_add(&zset, "c", 1, 30.0);
    zset_add(&zset, "d", 1, 30.0);
    zset_add(&zset, "e", 1, 40.0);
    
    std::vector<std::pair<std::string, double>> res;
    
    // Query score >= 20.0, limit 2
    zset_query(&zset, 20.0, "", 0, 0, 2, res);
    assert(res.size() == 2);
    assert(res[0].first == "b");
    assert(res[1].first == "c");
    
    // Query score >= 30.0, name >= "d"
    res.clear();
    zset_query(&zset, 30.0, "d", 1, 0, 2, res);
    assert(res.size() == 2);
    assert(res[0].first == "d");
    assert(res[1].first == "e");
    
    // Query with offset
    res.clear();
    zset_query(&zset, 10.0, "", 0, 2, 2, res); // skips "a", "b", returns "c", "d"
    assert(res.size() == 2);
    assert(res[0].first == "c");
    assert(res[1].first == "d");
    
    zset_destroy(&zset);
    printf("PASSED\n");
}

int main() {
    test_zset_basic();
    test_zset_duplicate_scores();
    test_zset_query();
    printf("All ZSet tests PASSED\n");
    return 0;
}
