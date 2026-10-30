# Deep Dive: System Architecture & Implementation Invariants

This document details the internal systems architecture of `byo-redis`, exploring the connection state machine, intrusive data structure memory topologies, the interaction between the single-threaded event loop and background worker threads, and failure recovery mechanics.

---

## 1. The Connection (`Conn`) State Machine & I/O Lifecycle

Every active TCP client connection is modeled by a `struct Conn` instance. The server operates strictly over non-blocking sockets using `poll` / `WSAPoll`.

```
                         [Client Connects]
                                │
                                ▼
                       ┌─────────────────┐
                       │   STATE_READ    │ <───────────────┐
                       └─────────────────┘                 │
                                │                          │
                    recv() readable data                   │
                                │                          │
                                ▼                          │
                       ┌─────────────────┐                 │
                       │  process_rbuf   │                 │
                       │ parse_request() │                 │
                       └─────────────────┘                 │
                                │                          │
                       do_request() produces reply         │
                                │                          │
                                ▼                          │
                    Append bytes to conn->wbuf             │
                                │                          │
                                ▼                          │
                       ┌─────────────────┐                 │
                       │   STATE_WRITE   │                 │
                       └─────────────────┘                 │
                                │                          │
                    WSAPoll() fires POLLOUT                │
                                │                          │
                                ▼                          │
                       ┌─────────────────┐                 │
                       │   state_write   │                 │
                       │  send() bytes   │                 │
                       └─────────────────┘                 │
                                │                          │
                    wbuf_sent == wbuf.size()               │
                                └──────────────────────────┘
```

### State Transitions & Buffer Management

1. **`STATE_READ`**:
   - The socket is registered in the poll set with `POLLIN`.
   - When readable, `state_read()` executes `recv()` into `conn->rbuf`.
   - If `recv()` returns `WSAEWOULDBLOCK` / `EWOULDBLOCK`, control returns to the event loop.
   - If `recv()` returns `0` (clean disconnect) or `SOCKET_ERROR` (connection reset), `conn->want_close = true`.

2. **Incremental Parsing (`process_rbuf`)**:
   - `parse_request()` scans `conn->rbuf` for complete RESP2 arrays (`*<count>\r\n...`) or inline commands.
   - If a command is incomplete, it returns `PARSE_NEED_MORE`, leaving partial bytes in `rbuf` until the next read event.
   - If the request violates the protocol, it returns `PARSE_ERR`, triggering immediate socket closure.
   - When a full command is parsed, `do_request()` processes it and serializes the reply into `conn->wbuf`.
   - `rbuf` consumes parsed bytes via `conn->rbuf.erase(0, consumed)`.

3. **`STATE_WRITE`**:
   - Once data is present in `wbuf`, the connection transitions to `STATE_WRITE`.
   - The poll set arms `POLLOUT` on the file descriptor.
   - `state_write()` issues non-blocking `send()`.
   - `wbuf_sent` tracks byte offsets. If `send()` delivers fewer bytes than remaining (`partial write`), `conn->state` remains `STATE_WRITE` and the event loop waits for the socket buffer to drain.
   - Once `wbuf_sent == wbuf.size()`, `conn->wbuf.clear()`, `conn->wbuf_sent = 0`, and the state switches back to `STATE_READ`.

4. **Connection Sweeping & Cleanup**:
   - In Step 6 of the event loop, any connection with `conn->want_close == true` is:
     1. Unsubscribed and purged from all Pub/Sub channels (`pubsub_clean_conn`).
     2. Detached from the idle timer list (`dlist_detach`).
     3. Closed via `closesocket()` / `close()`.
     4. Destroyed (`delete conn`).

---

## 2. Intrusive Data Structures & Memory Topology

Most standard implementations (e.g. `std::map`, `std::unordered_map`) allocate wrapper nodes on the heap for every insert, doubling pointer overhead and scattering cache lines. This project uses **intrusive data structures** inspired by the Linux kernel's `list_head`.

### The `container_of` Macro
```cpp
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
```
Given a pointer to an internal node (`HNode*`, `AVLNode*`, `DList*`), `container_of` computes the exact address of the outer struct via compile-time offset subtraction.

---

### Memory Layout: The Unified Keyspace Entry

A single `struct Entry` stores strings, TTL min-heap indices, and sorted sets without wrapper allocations:

```
+───────────────────────────────────────────────────────────────────────────+
| struct Entry (Heap Allocated, 64-bit alignment)                           |
|                                                                           |
|   +──────────────────────────+                                            |
|   | HNode node;              | <── Embedded in HMap bucket chain          |
|   |   HNode *next;           |                                            |
|   |   uint64_t hcode;        | <── Cached FNV-1a hash (avoids recompute)  |
|   +──────────────────────────+                                            |
|   | std::string key;         | <── Lookup key string                      |
|   | uint32_t type;           | <── T_STR (0) or T_ZSET (1)                |
|   | std::string val;         | <── String value (when type == T_STR)       |
|   | ZSet *zset;              | <── Sorted set pointer (type == T_ZSET)    |
|   | size_t heap_idx;         | <── Direct index in g_heap array           |
|   +──────────────────────────+                                            |
+───────────────────────────────────────────────────────────────────────────+
         ▲                                                │
         │ container_of(node, Entry, node)                │ Back-pointer
         │                                                ▼
+─────────────────+                             +───────────────────────────+
|  HMap (ht1/ht2) |                             | g_heap (std::vector)      |
|  Hash Table     |                             | [heap_idx] -> {val, &idx} |
+─────────────────+                             +───────────────────────────+
```

---

### Sorted Set (`ZSet`): Dual-Indexed Composite Topology

A sorted set requires simultaneous $O(1)$ member lookups by name and $O(\log N)$ range and rank queries by score.
Rather than storing two separate objects, each member is a single `struct ZNode`:

```
+───────────────────────────────────────────────────────────────────────────+
| struct ZNode                                                              |
|                                                                           |
|   +──────────────────────────+                                            |
|   | HNode hmap;              | <── Intrusive node for HMap hashtable      |
|   |   HNode *next;           |     Keyed by member name                   |
|   |   uint64_t hcode;        |     Enables O(1) ZSCORE, ZREM lookup       |
|   +──────────────────────────+                                            |
|   | AVLNode tree;            | <── Intrusive node for AVL tree            |
|   |   AVLNode *left;         |     Ordered by composite key (score, name) |
|   |   AVLNode *right;        |     Augmented with height & subtree size   |
|   |   AVLNode *parent;       |     Enables O(log N) ZRANK, ZQUERY         |
|   |   uint32_t height;       |                                            |
|   |   uint32_t cnt;          | <── Subtree node count (for rank queries)  |
|   +──────────────────────────+                                            |
|   | double score;            | <── Numerical sorting score                |
|   | size_t len;              | <── Member name string length              |
|   | char name[0];            | <── Flexible array member (inlined name)   |
|   +──────────────────────────+                                            |
+───────────────────────────────────────────────────────────────────────────+
```

```
     HMap (Name Index)                            AVL Tree (Score Index)
+─────────────────────────+                       (Ordered by score, name)
| [bucket] ──> ZNode.hmap |                                 [ZNode.tree]
+─────────────────────────+                                   /      \
                                                    [ZNode.tree]    [ZNode.tree]
```

#### Score Update Mechanics (`ZADD`)
Because the AVL tree order depends on `score`, mutating a score in-place violates the Binary Search Tree invariant. When `ZADD` updates an existing member's score:
1. Lookup `ZNode` via `HMap` by name in $O(1)$ time.
2. Detach `znode->tree` from the AVL tree via `avl_del()` in $O(\log N)$ time.
3. Update `znode->score = new_score`.
4. Re-insert `znode->tree` into the AVL tree at its new position in $O(\log N)$ time.
5. `znode->hmap` remains untouched in the hash table throughout the operation.

---

## 3. Algorithmic Invariants

### 1. Progressive Incremental Rehashing (`HMap`)
To prevent large-table resize latency spikes, rehashing is incremental across requests:
- When $\frac{\text{size}}{\text{mask} + 1} \ge 1.0$, `ht2` is allocated with $2 \times$ capacity.
- `resizing_idx` initializes to `0`.
- During every lookup, insertion, or deletion, `hm_trigger_rehashing()` migrates up to $128$ non-empty buckets from `ht1` to `ht2`.
- When all elements have migrated, `ht1` is deallocated, `ht1 = ht2`, and `ht2` resets to null.
- **Complexity**: $O(1)$ amortized per database operation.

### 2. Rank-Augmented AVL Balancing (`AVLNode.cnt`)
Each AVL node stores `cnt`: the total number of nodes in its subtree (including itself).
```
cnt(node) = 1 + cnt(node->left) + cnt(node->right)
```
- During rotations (LL, RR, LR, RL), `avl_update()` recalculates both `height` and `cnt`.
- **$O(\log N)$ Rank Query (`avl_offset`)**:
  - To find the $k$-th node in sorted order:
    - If $k < \text{cnt}(left)$, descend left.
    - If $k == \text{cnt}(left) + 1$, the current node is the target.
    - Otherwise, descend right searching for offset $k - (\text{cnt}(left) + 1)$.

### 3. Binary Min-Heap with Intrusive Back-Pointers (`HeapItem.ref`)
A naive heap requires $O(N)$ linear scans to find an item for TTL updates or key deletion.
- Every node in `g_heap` stores `{uint64_t val, size_t *ref}`.
- For each key, `ref` points directly to `&entry->heap_idx`.
- During any sift-up, sift-down, or swap in the heap array, the engine immediately updates:
  ```cpp
  *a[pos].ref = pos;
  ```
- **Guarantees**:
  - $O(1)$ earliest-expiration peek (`g_heap[0]`).
  - $O(\log N)$ expiration refresh on `EXPIRE` / `PEXPIRE` using the stored `entry->heap_idx`.
  - $O(\log N)$ arbitrary removal on `DEL` or overwrite without scanning.

---

## 4. The Thread Pool & Concurrency Architecture

Redis is famously single-threaded for request processing. Introducing multithreading into the core keyspace introduces locking overhead and data races. `byo-redis` enforces a strict **Detachment-Before-Handover Invariant**.

```
+─────────────────────────────────────────────────────────────────────────────+
|                         SINGLE-THREADED EVENT LOOP                          |
|                                                                             |
| 1. Client executes: DEL large_zset                                          |
| 2. hm_delete(&g_map, &ent->node)       <── Unlinked from hash table (O(1))  |
| 3. heap_delete(g_heap, ent->heap_idx)  <── Unlinked from TTL heap (O(log N))|
| 4. Is container size > 1,000 items?                                         |
|      YES: thread_pool_queue(&g_tp, &entry_del_async, ent)                   |
|      NO:  destroy and free immediately on main thread                       |
| 5. Return :1\r\n to client immediately                                      |
+─────────────────────────────────────────────────────────────────────────────+
                                       │
                                       │ Pointer Handover (Zero Database Locks)
                                       ▼
+─────────────────────────────────────────────────────────────────────────────+
|                          WORKER THREAD POOL (4 THREADS)                     |
|                                                                             |
| Worker Thread 2 dequeues task:                                              |
| 1. Takes exclusive ownership of detached `Entry*` pointer                   |
| 2. Recursively walks and deallocates millions of AVL tree nodes             |
| 3. Destroys member strings and frees memory                                 |
| 4. Main event loop never stalls, maintaining sub-millisecond tail latency   |
+─────────────────────────────────────────────────────────────────────────────+
```

### The Detachment-Before-Handover Invariant
1. Before any pointer is handed to `thread_pool_queue()`, the main thread **completely unlinks** it from all global data structures (`g_map` and `g_heap`).
2. Once unlinked, no subsequent command (`GET`, `SET`, `KEYS`) can observe or access that memory.
3. The worker thread acquires exclusive ownership of the detached memory graph.
4. **Result**: Zero mutexes, zero read-write locks, and zero race conditions on the core keyspace.

---

## 5. Persistence & Background Compaction Architecture

### Append-Only File (AOF) Engine
- Commands are written synchronously or buffered according to the fsync policy (`AOF_FSYNC_ALWAYS`).
- **Relative-to-Absolute Translation**: Commands like `EXPIRE key 60` are translated to `PEXPIREAT key <epoch_ms>` before writing to disk. When replayed on startup, expired keys are discarded, preventing key resurrection.
- **Crash Recovery**: `aof_load()` tracks the byte offset of the last complete RESP2 frame. If a power cut or crash creates a partial trailing command, the recovery engine automatically truncates the file back to `valid_bytes` using `SetEndOfFile` / `ftruncate`.

### Non-Forking Background AOF Compaction (`BGREWRITEAOF`)
Production Linux Redis uses `fork()` with copy-on-write. On Windows, `fork()` does not exist. `byo-redis` uses an in-memory snapshot and dual-buffering pipeline:

```
[Main Event Loop]
  │
  ├─ 1. aof_generate_snapshot_data(): Walk live keys in g_map and produce SET/ZADD/PEXPIREAT commands
  ├─ 2. Offload snapshot disk write & fsync to ThreadPool ("temp-appendonly.aof")
  ├─ 3. Set g_aof.rewrite_in_progress = true
  │
  │─── Concurrent Writes Arrive (e.g. SET k2 v2) ──────────────────────────┐
  │    ├─ Append to active AOF (ensures crash safety)                      │
  │    └─ Append to in-memory g_aof.rewrite_buf                            │
  │                                                                        │
[ThreadPool Background Worker]                                             │
  │                                                                        │
  └─ Writes snapshot bytes to temp file, calls fsync, exits                │
                                                                           │
[Main Event Loop: aof_check_rewrite_done()] <──────────────────────────────┘
  │
  ├─ 4. Flush g_aof.rewrite_buf into "temp-appendonly.aof"
  ├─ 5. Close active AOF descriptor
  ├─ 6. Atomically replace active file via MoveFileExA / rename
  └─ 7. Reset g_aof.rewrite_in_progress = false
```

---

## 6. Real-Time Pub/Sub Engine

Pub/Sub is decoupled from the key-value store and is completely ephemeral (never logged to AOF).

### Bidirectional Routing Topology
- **Global Channel Map**: `std::unordered_map<std::string, std::unordered_set<Conn*>> g_pubsub_channels`
  - $O(1)$ channel lookup on `PUBLISH`.
- **Per-Connection Subscription Set**: `Conn::channels` (`std::unordered_set<std::string>`)
  - Enables $O(M)$ cleanup on client disconnect (where $M$ is client subscriptions) rather than an $O(C \times S)$ scan over all server channels.

### Client Output Buffer Limit (COBL)
To prevent slow consumers from exhausting server memory during high-volume fan-out:
- Hard Limit: `k_pubsub_max_wbuf = 4 * 1024 * 1024` (4 MB).
- Before appending a published message to any subscriber's `wbuf`, the pending unwritten size is inspected:
  ```cpp
  if (sub->wbuf.size() - sub->wbuf_sent + msg_size > k_pubsub_max_wbuf) {
      sub->want_close = true; // Drop offender
      continue;
  }
  ```
- **Rationale**: Disconnecting the lagging client immediately is fail-fast; it protects server memory and ensures fast subscribers continue operating without latency degradation.

---

## 7. Complexity & Performance Invariants

| Operation | Data Structure | Time Complexity | Notes |
| :--- | :--- | :--- | :--- |
| `GET` / `SET` / `DEL` | `HMap` (Hashtable) | $O(1)$ amortized | Progressive rehashing bounds per-request latency |
| `EXPIRE` / `PERSIST` | `g_heap` (Min-Heap) | $O(\log N)$ | Direct index access via intrusive `size_t *ref` back-pointer |
| `ZRANGE` / `ZRANK` | `AVLNode` (AVL Tree) | $O(\log N)$ | Subtree count augmentation (`cnt`) allows tree rank queries |
| `ZSCORE` | `HMap` (ZSet member) | $O(1)$ | Direct hashtable lookup by member name string |
| Active TTL Sweep | Min-heap root eviction | $O(K \log N)$ | Evicts only the $K$ keys whose deadlines have expired |
| Large Deallocation | `ThreadPool` | $O(1)$ main thread stall | Detached from keyspace before background thread destruction |
| `PUBLISH` Fan-Out | `g_pubsub_channels` | $O(1) + O(S)$ | $O(1)$ channel map lookup + $O(S)$ iteration over $S$ subscribers |
| Client Disconnect | `Conn::channels` | $O(M)$ | Unlinks only the $M$ channels subscribed by this client |
