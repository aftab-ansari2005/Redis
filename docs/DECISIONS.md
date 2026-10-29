# Design Decisions Log

Track non-obvious choices, trade-offs, and bugs encountered during the build.
Format: Part N — what happened, what you chose, and why.

---

## Part 10 (Chapter 13) — TTL Expiration with a Binary Min-Heap

### 1. Intrusive Back-Pointers (`size_t *ref`) in the Min-Heap
- **Context:** In real Redis, keys with TTL are stored in a secondary hash table (`db->expires`). For an event loop with exact wakeups, we need a priority queue (min-heap) ordered by absolute expiration timestamp. However, naive heaps require an $O(n)$ linear scan to find a key when updating its TTL (`EXPIRE`), overwriting it (`SET`), or deleting it (`DEL`, `PERSIST`).
- **Choice:** Each `Entry` stores `size_t heap_idx = (size_t)-1`. The min-heap nodes store `{uint64_t val, size_t *ref}`. Whenever heap elements are moved (sift-up, sift-down, swap, or delete), the heap immediately updates `*a[pos].ref = pos`.
- **Result:** TTL refresh and arbitrary deletion are $O(\log n)$ instead of $O(n)$, with $O(1)$ lookup from the entry and zero hash table lookups during rebalancing.

### 2. Dual-Timer Event Loop Wakeup (`next_timer_ms`)
- **Context:** The event loop previously only considered idle connection timeouts from `g_idle_list`. If no traffic arrived, poll could sleep up to 10 seconds, letting expired keys linger past their deadlines.
- **Choice:** `next_timer_ms()` compares the earliest connection deadline with the root of `g_heap` (`g_heap[0].val`), taking the minimum:
  $$\text{next\_us} = \min(\text{conn\_deadline}, g\_heap[0].val)$$
- **Result:** The server wakes up on the exact millisecond when the next key expires to actively reclaim it in `process_timers()`, even under complete idle conditions with zero client packets.

### 3. Unified Lazy-Expiration Hook (`get_live_entry`)
- **Context:** Active expiration runs on timer ticks, but between ticks a client request could arrive for an expired key.
- **Choice:** Implemented `get_live_entry(key_str)` as the single point of entry for all command handlers (`GET`, `SET`, `DEL`, `EXISTS`, `KEYS`, `TTL`, `EXPIRE`, `PERSIST`, ZSet commands). If the key has `heap_idx != (size_t)-1` and `now_us >= expire_us`, it lazily purges the entry from both `g_map` and `g_heap` before returning `nullptr`.
- **Result:** Guarantees 100% semantic correctness — no client ever observes an expired key, regardless of active sweep timing.

---

## Part 11 (Chapter 14) — Thread Pool for Blocking Deallocations (Lazy Freeing)

### 1. Detachment-Before-Handover Invariant (Zero Locking on Main Keyspace)
- **Context:** In a single-threaded event loop, calling `DEL` on a collection with hundreds of thousands or millions of elements (like an AVL-based `ZSet`) blocks the event loop for hundreds of milliseconds while executing millions of `delete` invocations. To offload this to worker threads without complicating the single-threaded design with fine-grained keyspace mutexes, we must guarantee thread safety.
- **Choice:** Before handing an `Entry*` or `ZSet*` to `thread_pool_queue()`, the main thread *completely unlinks* it from the global hash table `g_map` (`hm_delete`) and from the TTL min-heap `g_heap` (`heap_delete`).
- **Result:** The detached data structure is completely isolated and invisible to subsequent event-loop queries (`GET`, `SET`, `DEL`, etc.). The worker thread takes exclusive ownership of the memory and destroys it concurrently without needing any locks on the main database.

### 2. Deallocation Cost Threshold (`k_large_container_size`)
- **Context:** Submitting tasks to a thread pool incurs synchronization overhead (mutex lock + condvar signal). For small values or collections with few members, synchronous freeing is significantly faster than thread pool queue contention.
- **Choice:** Added a threshold check (`k_large_container_size = 1000`). If a ZSet has $> 1000$ members, it is offloaded to the thread pool; otherwise, it is destroyed immediately on the event-loop thread.
- **Result:** Preserves high throughput for normal small-key workloads while preventing catastrophic tail latency spikes on mega-key destructions.

### 3. Cross-Platform Threading Primitives (Win32 & POSIX)
- **Context:** MinGW on Windows with standard `win32` thread model does not support C++11 `<thread>`/`<mutex>`.
- **Choice:** Implemented `ThreadPool` with conditional compilation supporting native Windows synchronization (`CRITICAL_SECTION`, `CONDITION_VARIABLE`, `_beginthreadex`) as well as standard POSIX (`pthread_mutex_t`, `pthread_cond_t`, `pthread_create`).
- **Result:** Fully portable across Windows (MinGW/MSVC) and Linux/macOS with zero external runtime dependencies.

---

## Part 12 (Extension) — Append-Only File (AOF) Persistence

### 1. Canonical RESP2 Array Wire Format for AOF
- **Context:** Storing commands in custom binary or plaintext formats causes code duplication, maintenance overhead, and custom parser complexity.
- **Choice:** Used standard RESP2 arrays (`*<argc>\r\n$<len>\r\n...`) identical to client requests. Replay directly invokes `parse_request()`.
- **Result:** Zero parser duplication; 100% format alignment with client communication.

### 2. Relative TTL Expiration Translation (`PEXPIREAT`)
- **Context:** Logging `EXPIRE key 60` causes expiration countdown to restart on server reboot, resurrecting keys that should have died during downtime.
- **Choice:** Translated all `EXPIRE` and `PEXPIRE` commands to `PEXPIREAT key <unix_timestamp_ms>` before writing to disk. Replay checks `unix_timestamp_ms <= now_ms` and discards expired keys immediately, preventing resurrection.
- **Result:** Full time-consistency and correct expiration semantics across arbitrary crash/restart intervals.

### 3. Graceful Crash Recovery via Tail Truncation
- **Context:** Process termination or power failure can interrupt a disk write, leaving an incomplete command at the end of the file.
- **Choice:** `aof_load()` tracks the byte offset of the last complete command (`valid_bytes`). If `parse_request()` returns `PARSE_NEED_MORE` or `PARSE_ERR` at EOF, the file is automatically truncated back to `valid_bytes` using `SetEndOfFile` / `ftruncate()`.
- **Result:** Self-healing startup that preserves all committed operations without manual administrative intervention.

### 4. Non-Forking Background Compaction (`BGREWRITEAOF`) & Rewrite Buffer
- **Context:** An append-only log monotonically accumulates command history (e.g. 100,000 updates to a single key). Linux Redis uses POSIX `fork()` with Copy-on-Write to dump memory without blocking, but Windows lacks `fork()`.
- **Choice:** Implemented an in-memory snapshot generator that walks live keys in `g_map` and produces minimal reconstruction commands (`SET`, `ZADD`, `PEXPIREAT`). The heavy disk I/O and `fsync` are offloaded to `ThreadPool`.
- **Concurrent Writes:** Mutating writes arriving while the background worker is active are dual-written: flushed to the active AOF (preserving crash safety) and buffered in `g_aof.rewrite_buf`. When the worker finishes, the event loop appends the rewrite buffer to the temp file, closes the active AOF, and atomically swaps it in via `MoveFileExA(..., MOVEFILE_REPLACE_EXISTING)`.
- **Poll Clamping:** Clamped `WSAPoll` timeout to 50ms while `rewrite_in_progress` is active, ensuring the event loop completes the rewrite immediately upon worker exit without waiting for idle socket timeouts.
- **Result:** Zero event-loop blocking, zero data races on keyspace memory, and zero lost writes under heavy concurrent traffic.


