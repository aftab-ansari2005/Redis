#pragma once

#include <stddef.h>

// --------------------------------------------------------------------------
// Intrusive Doubly-Linked List (DList)
//
// Like Linux's struct list_head, this struct is embedded directly inside
// the objects that belong to the list. This avoids separate allocations for
// list nodes and allows O(1) removal from an arbitrary position in the list
// without having to search from the head.
//
// The list is circular with a dummy sentinel node:
// - Empty list: head.next == &head && head.prev == &head
// - First item: head.next
// - Last item:  head.prev
// --------------------------------------------------------------------------
struct DList {
    DList *prev = NULL;
    DList *next = NULL;
};

// Initialize an empty list head or isolated node
inline void dlist_init(DList *node) {
    node->prev = node->next = node;
}

// Check if list is empty (sentinel node points to itself)
inline bool dlist_empty(DList *node) {
    return node->next == node;
}

// Detach a node from its current list.
// Safe to call multiple times (idempotent).
inline void dlist_detach(DList *node) {
    if (!node->prev || !node->next || node->next == node) {
        return;
    }
    DList *prev = node->prev;
    DList *next = node->next;
    prev->next = next;
    next->prev = prev;
    node->prev = node->next = NULL;
}

// Insert rookie immediately before target.
// When target is the list sentinel &head, this appends rookie to the TAIL
// of the list (since &head's predecessor is the current tail).
inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}
