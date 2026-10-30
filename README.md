# Redis from Scratch (`byo-redis`)

A high-performance, in-memory key-value data store built from scratch in C++14. Implements a single-threaded non-blocking I/O event loop on top of `poll`/`WSAPoll`, custom intrusive data structures with progressive rehashing, rank-augmented AVL trees for sorted sets, exact-millisecond TTL eviction using a min-heap with intrusive back-pointers, worker thread pool for non-blocking asynchronous deallocations, full RESP2 wire protocol compatibility (`redis-cli`), AOF persistence with self-healing crash recovery and non-forking background compaction, transactional atomicity (`MULTI`/`EXEC`/`DISCARD`), and an asynchronous Pub/Sub messaging bus with buffer protection against slow consumers.

Designed and engineered from first principles without external database frameworks or third-party container libraries.

---

## Architecture & Data Flow

```
                                  +-----------------------------+
                                  |   redis-cli / TCP Clients   |
                                  +-----------------------------+
                                                 │
                                                 │ [RESP2 Protocol: *3\r\n$3\r\nSET...]
                                                 ▼
+─────────────────────────────────────────────────────────────────────────────────────────────+
│                               SINGLE-THREADED EVENT LOOP                                    │
│                                                                                             │
│   1. Compute Timeout           2. WSAPoll() / poll()          3. Accept & Read              │
│   ┌─────────────────────┐      ┌─────────────────────┐      ┌───────────────────────────┐   │
│   │ min(idle_deadline,  │ ───> │ Wait for socket I/O │ ───> │ recv() into conn->rbuf    │   │
│   │     heap[0].expiry) │      │ or timer expiration │      │ parse_request()           │   │
│   └─────────────────────┘      └─────────────────────┘      └───────────────────────────┘   │
│                                                                           │                 │
│                                                                           ▼                 │
│   6. State Machine Flush       5. Pub/Sub Fan-Out           4. Command Dispatcher           │
│   ┌─────────────────────┐      ┌─────────────────────┐      ┌───────────────────────────┐   │
│   │ send(conn->wbuf)    │ <─── │ Append push payloads│ <─── │ do_request()              │   │
│   │ Transition to READ  │      │ to subscriber wbufs │      │ Enforce Subscribed Mode   │   │
│   └─────────────────────┘      └─────────────────────┘      └─────────────┬─────────────┘   │
│                                                                           │                 │
+───────────────────────────────────────────────────────────────────────────┼─────────────────+
                                                                            │
                          ┌─────────────────────────────────────────────────┴─────────────┐
                          ▼                                                               ▼
        +───────────────────────────────────+                           +───────────────────────────────────+
        |        PRIMARY KEYSPACE           |                           |       ACTIVE TTL MIN-HEAP         |
        |  HMap: 2 HTabs (Chaining)         |                           |  std::vector<HeapItem>            |
        |  Progressive incremental rehash   |                           |  Sorted by monotonic microsecond  |
        +───────────────────────────────────+                           +───────────────────────────────────+
                          │                                                               │
                          │ [Entry embeds HNode, HeapItem index]                          │ [size_t *ref back-pointer]
                          ▼                                                               ▼
        +───────────────────────────────────────────────────────────────────────────────────────────────────+
        |                                        ENTRY INTRUSIVE NODE                                       |
        |  struct Entry {                                                                                   |
        |      HNode    node;      ──> Embedded singly-linked list node in HMap                             |
        |      size_t   heap_idx;  <── Direct index in TTL heap (O(1) lookup, O(log N) TTL update/delete)   |
        |      uint32_t type;      ──> T_STR or T_ZSET                                                      |
        |      string   val;       ──> String payload (when type == T_STR)                                  |
        |      ZSet*    zset;      ──> Sorted set pointer (when type == T_ZSET)                             |
        |  };                                                                                               |
        +───────────────────────────────────────────────────────────────────────────────────────────────────+
                          │                                                               │
                          ▼                                                               ▼
        +───────────────────────────────────+                           +───────────────────────────────────+
        |        SORTED SET (ZSET)          |                           |     PERSISTENCE & DEALLOCATION    |
        |  Dual-indexed ZNode:              |                           |                                   |
        |  1. HMap: name -> ZNode (O(1))    |                           |  AOF: Append-Only File Engine     |
        |  2. AVL Tree: (score, name)       |                           |  - Contiguous MULTI/EXEC blocks   |
        |     augmented with subtree size   |                           |  - PEXPIREAT absolute timestamps  |
        |     for O(log N) rank queries     |                           |  - Self-healing tail truncation   |
        +───────────────────────────────────+                           +───────────────────────────────────+
                          │                                                               │
                          │ [Container size > 1,000 items]                                │ [Background rewrite]
                          ▼                                                               ▼
        +───────────────────────────────────────────────────────────────────────────────────────────────────+
        |                                      WORKER THREAD POOL (4 THREADS)                               |
        |  - Async Lazy Freeing: Detach entry from keyspace first, free millions of nodes concurrently      |
        |  - Background AOF Compaction: In-memory snapshot generation + thread-pool disk streaming & fsync  |
        +───────────────────────────────────────────────────────────────────────────────────────────────────+
```

---

## Feature Overview

### 1. Non-Blocking I/O & Event Loop
- Edge/level-triggered multiplexing using `poll` (Linux/macOS) and `WSAPoll` (Windows).
- Explicit `Conn` state machine (`STATE_READ` $\leftrightarrow$ `STATE_WRITE`) with pipelined request buffering.
- Dual-deadline timer coalescing: computes poll sleep duration as $\min(\text{idle\_timeout}, \text{earliest\_ttl})$ to eliminate busy waiting and avoid missed expirations.

### 2. Custom Chaining Hash Table with Progressive Rehashing
- Built on the intrusive node pattern (`HNode`, `container_of`).
- Dual table structure (`HMap` wrapping `ht1` and `ht2`): dynamically resizes when load factor $> 1.0$.
- Resizing is non-blocking and incremental: every read/write/delete migrates a fixed chunk of buckets ($128$ buckets/op), bounding latency to $O(1)$ amortized per request.

### 3. Dual-Indexed Sorted Set (`ZSet`) via Augmented AVL Tree
- Each `ZNode` simultaneously embeds an `HNode` and an `AVLNode` in a single allocation.
- **$O(1)$ Membership**: Direct member lookup, score query, and existence checks via `HMap`.
- **$O(\log N)$ Rank & Range Queries**: AVL tree ordered by composite key `(score, name)`. Every node tracks its subtree size (`cnt`), allowing $O(\log N)$ rank lookups (`ZRANK`) and range slicing with offset/limit (`ZQUERY`).

### 4. Active & Lazy TTL Expiration via Priority Min-Heap
- **Priority Min-Heap**: Orders key expirations by monotonic microsecond timestamps.
- **Intrusive Back-Pointers (`size_t *ref`)**: When elements swap or move during sift operations, the heap updates `*ref` immediately. Enables $O(\log N)$ TTL updates (`EXPIRE`/`PEXPIRE`), $O(\log N)$ arbitrary removals (`DEL`/`PERSIST`), and $O(1)$ earliest-expiry inspection.
- **Unified Expiration Hook**: `get_live_entry()` lazily purges expired keys on demand, guaranteeing zero stale reads between active active-timer sweeps.

### 5. Asynchronous Deallocation Thread Pool (`lazyfree`)
- 4 worker threads managed with condition variables and job queues.
- **Detachment-Before-Handover Invariant**: Large keys ($> 1,000$ members) are unlinked from the global keyspace in $O(1)$ time on the main thread before pointer handover to the worker pool.
- Zero mutex contention on the main database keyspace while megabyte/gigabyte collections are deallocated concurrently.

### 6. Append-Only File (AOF) Persistence
- Canonical RESP2 array command serialization.
- **Absolute Expiration Translation**: Translates relative TTL commands (`EXPIRE 60`) to absolute epoch timestamps (`PEXPIREAT <ms>`) to eliminate key resurrection on restart.
- **Self-Healing Recovery**: Automatically detects incomplete/corrupted writes at EOF during startup and truncates the file back to the last valid byte offset.
- **Non-Forking Background Compaction (`BGREWRITEAOF`)**: Traverses the live database to generate minimal reconstruction commands, offloading file writing and physical disk synchronization (`fsync`) to the thread pool while buffering concurrent writes for seamless atomic swap.

### 7. Atomic Transactions (`MULTI` / `EXEC` / `DISCARD`)
- Pipelined command queuing per connection.
- **Two-Phase Validation**: Pre-validates command syntax and arity at queue time (`EXECABORT`).
- **Atomic AOF Logging**: Emits transaction mutations as a single contiguous `MULTI ... EXEC` block with complete rollback on truncated crash recovery.

### 8. Real-Time Pub/Sub Messaging Bus
- Dual-indexed routing: `channel -> Set<Conn*>` and `Conn -> Set<channel>`.
- Supports exact channel subscriptions (`SUBSCRIBE`/`UNSUBSCRIBE`) and glob pattern matching (`PSUBSCRIBE`/`PUNSUBSCRIBE`/`PMESSAGE`).
- **Subscribed Mode Enforcement**: Rejects non-pubsub commands in RESP2 mode to prevent framing collisions on half-duplex client connections.
- **Slow Consumer Protection**: Hard client output buffer limits ($4\text{ MB}$) that disconnect lagging subscribers before server memory is compromised.

---

## What I Built vs. The Book vs. Real Redis

| Dimension | *Build Your Own Redis* (Book) | `byo-redis` (This Project) | Production Redis 7.x |
| :--- | :--- | :--- | :--- |
| **I/O Model** | `poll()` single-threaded | Cross-platform `poll()` / `WSAPoll` with state machine | Single-threaded event loop (`epoll`/`kqueue`/`io_uring`) + Threaded I/O |
| **Protocol** | Custom length-prefixed binary protocol | Full **RESP2 wire protocol** (native `redis-cli`) | RESP2 & RESP3 with push streaming frames |
| **Hash Table** | Chaining + progressive rehashing | Chaining + progressive rehashing + intrusive `container_of` | Chaining + progressive rehashing + dict zip compression |
| **Sorted Sets** | AVL tree (with height balancing) | **Augmented AVL tree** with subtree sizes (`cnt`) for rank queries | Skip List (`zskiplist`) + Hash Table |
| **TTL Eviction** | Linear scan or simple timers | **Binary min-heap with intrusive back-pointers** (`size_t *ref`) | 24-bit LRU/LFU clock + random 20-key active sampling |
| **Concurrency** | Single-threaded | **Thread Pool** for async `lazyfree` and background AOF rewrite | Main thread + background BIO threads + optional I/O threads |
| **Persistence** | None | **AOF engine** with `PEXPIREAT` translation, tail repair & `BGREWRITEAOF` | RDB snapshots (`fork()`) + AOF with multi-part manifests |
| **Transactions** | Not covered | **`MULTI` / `EXEC` / `DISCARD`** with `EXECABORT` validation | `MULTI` / `EXEC` / `DISCARD` / `WATCH` (optimistic locking) |
| **Pub/Sub** | Not covered | **Full Pub/Sub + Pattern Matching** + Client Output Buffer Limits | Pub/Sub + Sharded Pub/Sub (`SSUBSCRIBE` in Redis 7) |
| **Platform** | Linux-first | Fully cross-platform (**Windows, Linux, macOS, Docker**) | Linux/POSIX optimized (Windows port discontinued) |

---

## Supported Commands

| Category | Commands |
| :--- | :--- |
| **Strings & Keys** | `GET`, `SET`, `DEL`, `UNLINK`, `EXISTS`, `KEYS` |
| **TTL & Expiration** | `EXPIRE`, `PEXPIRE`, `PEXPIREAT`, `TTL`, `PTTL`, `PERSIST` |
| **Sorted Sets** | `ZADD`, `ZSCORE`, `ZREM`, `ZRANK`, `ZQUERY`, `ZPOPULATE` |
| **Transactions** | `MULTI`, `EXEC`, `DISCARD` |
| **Pub/Sub** | `SUBSCRIBE`, `UNSUBSCRIBE`, `PSUBSCRIBE`, `PUNSUBSCRIBE`, `PUBLISH` |
| **Persistence & Admin** | `BGREWRITEAOF`, `REWRITEAOF`, `CONFIG GET/SET lazyfree`, `PING`, `ECHO`, `QUIT`, `RESET`, `CLIENT`, `COMMAND` |

---

## Build & Run

### Prerequisites
- C++14 capable compiler (`g++` $\ge 5.0$, `clang++` $\ge 3.8$, or MSVC 2017+)
- `make` or `mingw32-make`
- Docker (optional)

### Local Build (Linux / macOS / Windows MinGW)

```bash
# Clone the repository
git clone https://github.com/aftab-ansari2005/byo-redis.git
cd byo-redis

# Compile server and all test suites
make all

# Start the server on port 1234 with AOF persistence enabled
./server.exe --aof appendonly.aof -p 1234
# (On Linux / macOS: ./server --aof appendonly.aof -p 1234)
```

### Connect with Official `redis-cli`

In a separate terminal, connect using the official Redis CLI:

```bash
$ redis-cli -p 1234
127.0.0.1:1234> SET user:1001 "Alice"
OK
127.0.0.1:1234> EXPIRE user:1001 60
(integer) 1
127.0.0.1:1234> TTL user:1001
(integer) 58
127.0.0.1:1234> ZADD leaderboard 1500 "Alice"
(integer) 1
127.0.0.1:1234> ZADD leaderboard 2100 "Bob"
(integer) 1
127.0.0.1:1234> ZRANK leaderboard "Bob"
(integer) 1
127.0.0.1:1234> ZQUERY leaderboard 0 "" 0 10
1) "Alice"
2) "1500"
3) "Bob"
4) "2100"
```

### Run with Docker

```bash
# Build the container image
docker build -t byo-redis .

# Run the container exposing port 1234
docker run -d -p 1234:1234 --name redis-server byo-redis

# Test via redis-cli
redis-cli -p 1234 PING
```

---

## Test Suite

The test suite covers unit correctness, concurrency, protocol edge cases, and crash recovery:

```bash
# Data Structure Unit Tests (Isolated from Networking)
./test_hashtable.exe    # Incremental resizing, collision stress, deletion during migration
./test_avl.exe          # Rotations (LL, RR, LR, RL), rank queries, deletion rebalance, fuzzing
./test_zset.exe         # Dual-structure synchronization, duplicate scores, score updates
./test_heap.exe         # TTL min-heap invariant, back-pointer index tracking, sift up/down
./test_dlist.exe        # Intrusive doubly linked list circular sentinel operations
./test_thread_pool.exe  # Worker concurrency, queue synchronization, shutdown barrier

# System & Protocol Integration Tests
./test_commands.exe     # End-to-end command execution
./test_concurrent.exe   # Multi-client concurrent connections & pipelining
./test_ttl.exe          # Active vs lazy expiration verification under wall-clock delay
./test_lazyfree.exe     # Large container async thread-pool deallocation verification
./test_transactions.exe # MULTI/EXEC/DISCARD, EXECABORT syntax validation, runtime isolation
./test_aof.exe          # Persistence replay, tail truncation repair, BGREWRITEAOF concurrency
./test_pubsub.exe       # Fan-out, pattern matching, subscribed mode, slow consumer drop
```

---

## License

MIT License. Designed and built for systems programming education and portfolio showcase.
