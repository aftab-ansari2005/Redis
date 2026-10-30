# Systems Interview Prep Guide: `byo-redis`

This document contains 5 deep technical interview questions tailored specifically to the architecture, data structures, concurrency, and persistence invariants built into this project, complete with production-grade model answers.

---

### Question 1: Concurrency Model — The Single-Threaded Event Loop vs. Multi-Threading

**Question:**
> *"Why did you design your Redis clone with a single-threaded event loop (`poll`/`WSAPoll`) instead of a thread-per-connection or thread-pool architecture? Modern server hardware has 64+ CPU cores—aren't you leaving massive throughput on the table by keeping request processing single-threaded?"*

#### Strong Model Answer:
"In-memory key-value databases are almost never CPU-bound on computation—they are **memory-bandwidth, cache-coherency, and network I/O-bound**.

If we adopted a multi-threaded architecture with worker threads directly mutating a shared hash table:
1. **Synchronization Overhead & Cache Bouncing**: Every `GET`, `SET`, or `DEL` would require fine-grained bucket mutexes or concurrent skiplists. When multiple CPU cores simultaneously acquire locks and invalidate cache lines on shared memory addresses (cache coherency traffic via MESI protocols), throughput drops drastically, and tail latency ($\text{P99.9}$) spikes due to lock contention.
2. **Context Switching & Stack Footprint**: A thread-per-connection model does not scale past a few thousand clients. $10,000$ idle threads with default $8\text{ MB}$ stack allocations consume $80\text{ GB}$ of virtual memory just for stacks, overwhelming the OS scheduler.
3. **Deterministic Atomicity**: A single-threaded event loop provides atomic operations by default. Commands like `INCR`, `DEL`, and `MULTI`/`EXEC` execute without requiring distributed consensus or multi-version concurrency control (MVCC).

**How I address multi-core utilization**:
Rather than making the keyspace multi-threaded, Redis scales in two ways:
- **Horizontal Scaling**: Run independent Redis instances pinned to individual CPU cores via CPU affinity, sharded by key hash slots.
- **Selective Asynchronous Offloading**: In my project, CPU-intensive background tasks that would otherwise block the event loop are offloaded to a 4-worker `ThreadPool`:
  - Large container deallocations (`lazyfree` of $> 1,000$ members)
  - Background AOF persistence compaction (`BGREWRITEAOF` disk streaming and `fsync`)

This architecture keeps the request-processing hot path completely lock-free while insulating event-loop latency from heavy background tasks."

---

### Question 2: Memory Topologies — Intrusive Data Structures vs. Standard Containers

**Question:**
> *"Your `Entry` and `ZNode` structs embed intrusive nodes (`HNode`, `AVLNode`, `DList`) and use the `container_of` macro rather than standard C++ containers like `std::unordered_map` or `std::map`. Why did you choose intrusive data structures, and what concrete memory and cache benefits do they offer?"*

#### Strong Model Answer:
"`std::unordered_map` and `std::map` are non-intrusive container wrappers. Using them creates severe memory and cache inefficiencies:

1. **Elimination of Secondary Allocations**:
   - In a non-intrusive `std::map<string, double>`, every insert performs an independent heap allocation for the internal tree node, which holds pointers to a dynamically allocated key string and value. Storing 1,000,000 members produces at least 2,000,000 to 3,000,000 separate heap allocations, resulting in heap fragmentation and malloc metadata overhead ($16\text{ bytes}$ per chunk).
   - In my intrusive design, a sorted set member is represented by a single `ZNode` allocation. The string name is stored using a flexible array member (`char name[0]`), and the struct directly embeds both the `HNode` (hash table bucket pointer) and the `AVLNode` (tree pointers). **One allocation per member covers all indices**.

2. **Simultaneous Multi-Container Membership**:
   - A single `ZNode` lives inside the `HMap` hashtable (keyed by member name for $O(1)$ lookups) and simultaneously inside the `AVLNode` tree (ordered by `(score, name)` for $O(\log N)$ rank queries).
   - Because the data structures store raw intrusive pointers directly into the node, moving between the hash table view and the tree view requires zero hash table lookups and zero pointer chasing—it is an instantaneous $O(1)$ address calculation via `container_of`.

3. **In-Place Re-indexing on Score Updates**:
   - When a member's score changes via `ZADD`, we cannot mutate the score in-place because it would violate the AVL tree ordering.
   - With our intrusive design, we locate the `ZNode` via the hash table in $O(1)$, detach `znode->tree` via `avl_del()`, update `znode->score`, and re-insert into the AVL tree in $O(\log N)$. The member name and `znode->hmap` node in the hash table remain completely untouched."

---

### Question 3: Tail-Latency Spikes — Progressive Rehashing & The Detachment Invariant

**Question:**
> *"In production systems, hash table resizing and large key deletions are the two most notorious causes of latency spikes. How does your hash table avoid stop-the-world resizing, and how does your server handle a `DEL` command on a sorted set containing 1,000,000 elements without freezing concurrent traffic?"*

#### Strong Model Answer:
"Both problems are solved by bounding work per event-loop iteration:

**1. Incremental Progressive Rehashing (`HMap`)**:
- Standard hash tables reallocate a new bucket array and rehash all entries in a single synchronous loop. For a table with 5,000,000 keys, this freezes the server for hundreds of milliseconds.
- My `HMap` uses two internal tables: `ht1` (old, shrinking) and `ht2` (new, doubling). When the load factor exceeds 1.0, rehashing begins.
- Every read, write, or delete operation calls `hm_trigger_rehashing()`, which migrates exactly **128 non-empty buckets** from `ht1` to `ht2`.
- Lookups check `ht1` first, then `ht2` if not found.
- The cost of migration is amortized across incoming client requests, bounding per-request latency to $O(1)$ without latency cliffs.

**2. Asynchronous Lazy Freeing & The Detachment Invariant**:
- Deleting an AVL tree with 1,000,000 nodes requires walking the tree and invoking `delete` 1,000,000 times, which consumes ~150 ms of pure CPU time on the main thread.
- To prevent this stall, I enforce the **Detachment-Before-Handover Invariant**:
  1. On `DEL key`, the event loop checks the container size. If $> 1,000$ elements, lazy freeing is triggered.
  2. The main thread immediately unlinks the `Entry` from `g_map` (`hm_delete`) and from the TTL min-heap `g_heap` (`heap_delete`) in $O(1)$ time.
  3. Once unlinked, the entry is completely invisible to subsequent event-loop queries (`GET`, `SET`, `KEYS`).
  4. The pointer is passed to the background `ThreadPool`. A worker thread takes exclusive ownership and recursively destroys the AVL tree nodes concurrently.
- Because the memory was completely detached before handoff, **zero mutexes or read-write locks are required on the global keyspace**."

---

### Question 4: Crash Recovery & Persistence — AOF Engine & Monotonic Expirations

**Question:**
> *"Suppose your server is under heavy write traffic and experiences a sudden power cut midway through writing an AOF command block, or during a background compaction (`BGREWRITEAOF`). What corruption occurs, how does your startup replay engine recover without data loss, and why don't keys with relative TTLs (`EXPIRE key 60`) get resurrected on reboot?"*

#### Strong Model Answer:
"This touches three critical persistence invariants:

**1. Self-Healing Tail Truncation**:
- A crash mid-write leaves a partial RESP2 frame at the end of `appendonly.aof` (e.g. `*3\r\n$3\r\nSET\r\n$4\r\nuser...` with missing bytes).
- During startup, `aof_load()` reads the file sequentially, tracking `valid_bytes`—the exact byte offset of the last fully parsed, valid command.
- When `parse_request()` hits EOF and returns `PARSE_NEED_MORE` or `PARSE_ERR`, the server detects that the final command was interrupted.
- It automatically truncates the corrupted tail back to `valid_bytes` using `ftruncate()` (POSIX) or `SetEndOfFile()` (Windows) and logs a warning. All completed, fsynced operations preceding the crash are replayed, and corrupted trailing fragments are purged.

**2. Preventing Key Resurrection via Absolute Timestamps (`PEXPIREAT`)**:
- If an AOF logged `EXPIRE key 60`, replaying that file on a server restart 2 hours later would restart the 60-second countdown, resurrecting a key that expired 1 hour and 59 minutes ago.
- To solve this, my server converts all relative expiration commands (`EXPIRE`, `PEXPIRE`) to **absolute Unix epoch milliseconds** (`PEXPIREAT key <timestamp_ms>`) before writing to the AOF.
- During AOF startup replay, `do_pexpireat()` compares `<timestamp_ms>` against the current wall-clock time (`get_wall_clock_ms()`). If expired, the entry is discarded immediately without insertion.

**3. Atomic Snapshot Swap during `BGREWRITEAOF`**:
- During compaction, the background worker streams an in-memory snapshot to a temporary file (`temp-appendonly.aof`).
- Live mutating writes arriving while the worker is busy are dual-written: flushed to the active AOF (preserving crash safety) and buffered in memory in `g_aof.rewrite_buf`.
- When the worker finishes, the event loop appends the rewrite buffer to the temp file, closes the active AOF descriptor, and performs an atomic filesystem replacement (`rename()` on POSIX, `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)` on Windows). If a crash happens at any microsecond before the rename, the original AOF remains intact."

---

### Question 5: Real-Time Messaging — The Slow Consumer Problem in Pub/Sub

**Question:**
> *"In a Pub/Sub system where one connection publishes messages to a channel with 5,000 subscribers, what happens if one subscriber stops reading from its socket or experiences network congestion? Walk through the failure modes of blocking, lossy dropping, and disconnection, and explain why Client Output Buffer Limits are the industry-standard solution."*

#### Strong Model Answer:
"In an asynchronous event loop, writing to other connections breaks the 1:1 request-response boundary. When client $A$ publishes, the server fans out the payload by appending to the write buffer (`wbuf`) of subscribers $B_1 \dots B_{5000}$ and arming `POLLOUT`.

If subscriber $B_{42}$ freezes or stops reading:
1. $B_{42}$'s TCP receive window fills up.
2. The server's OS socket send buffer fills up, and `send()` returns `WSAEWOULDBLOCK` / `EWOULDBLOCK`.
3. If publishers continue publishing at $10,000\text{ msg/sec}$, unwritten messages accumulate in $B_{42}$'s userspace buffer (`conn->wbuf`).
4. Without protection, `wbuf` grows to gigabytes until the server crashes with Out Of Memory (OOM), taking down all clients.

**Evaluating the Architectural Mitigations**:
- **Option A: Blocking / Backpressure**: The event loop halts and waits for $B_{42}$'s socket to drain.
  *Why it fails*: In a single-threaded server, blocking to accommodate one slow subscriber stalls the entire database—halting all unrelated `GET` and `SET` operations across the organization.
- **Option B: Silent Dropping (Lossy Ring Buffer)**: Drop unread messages when the subscriber's buffer is full.
  *Why it fails*: Silently dropping messages violates stream integrity. If a subscriber is tracking state, it silently falls out of sync without notification, leading to hard-to-debug data corruption.
- **Option C: Fail-Fast Eviction (Client Output Buffer Limits)**:
  *Why it succeeds*: My engine enforces a hard limit `k_pubsub_max_wbuf = 4 MB`. If a subscriber's pending unwritten bytes exceed 4 MB, the server marks `sub->want_close = true` and closes its socket in the event loop.

By evicting the slow consumer, server memory is protected, healthy subscribers continue operating without latency jitter, and the slow client receives an explicit TCP reset, immediately signaling to the application that it was disconnected and must reconnect and re-sync."

