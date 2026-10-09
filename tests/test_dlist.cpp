#include <cstdio>
#include <cstdlib>
#include <cassert>
#include "../src/dlist.h"
#include "../src/hashtable.h" // for container_of

struct Item {
    int val;
    DList node;
};

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: %s\n", __LINE__, msg); \
            exit(1); \
        } \
    } while (0)

static void test_dlist_basic() {
    DList head;
    dlist_init(&head);
    CHECK(dlist_empty(&head), "Newly initialized list should be empty");

    Item a = {1, {}};
    Item b = {2, {}};
    Item c = {3, {}};

    // Append to tail by inserting before &head
    dlist_insert_before(&head, &a.node);
    CHECK(!dlist_empty(&head), "List should not be empty after insert");
    dlist_insert_before(&head, &b.node);
    dlist_insert_before(&head, &c.node);

    // List order should be a -> b -> c
    DList *cur = head.next;
    Item *item = container_of(cur, Item, node);
    CHECK(item->val == 1, "First element should be 1");

    cur = cur->next;
    item = container_of(cur, Item, node);
    CHECK(item->val == 2, "Second element should be 2");

    cur = cur->next;
    item = container_of(cur, Item, node);
    CHECK(item->val == 3, "Third element should be 3");

    cur = cur->next;
    CHECK(cur == &head, "Iteration should loop back to head");

    // Remove middle element (b)
    dlist_detach(&b.node);

    cur = head.next;
    CHECK(container_of(cur, Item, node)->val == 1, "Head should be 1");
    cur = cur->next;
    CHECK(container_of(cur, Item, node)->val == 3, "Next should be 3");
    cur = cur->next;
    CHECK(cur == &head, "Should reach head");

    // Move a to tail: detach then insert_before(&head)
    dlist_detach(&a.node);
    dlist_insert_before(&head, &a.node);

    cur = head.next;
    CHECK(container_of(cur, Item, node)->val == 3, "New head should be 3");
    cur = cur->next;
    CHECK(container_of(cur, Item, node)->val == 1, "New tail should be 1");

    // Detach all
    dlist_detach(&c.node);
    dlist_detach(&a.node);
    CHECK(dlist_empty(&head), "List should be empty after detaching all");

    // Test idempotent detach (calling detach again should be safe)
    dlist_detach(&a.node);
    dlist_detach(&b.node);
    dlist_detach(&c.node);

    printf("test_dlist_basic PASSED\n");
}

int main() {
    test_dlist_basic();
    printf("All DList tests PASSED!\n");
    return 0;
}
