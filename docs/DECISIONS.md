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


