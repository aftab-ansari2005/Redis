# Build-Your-Own-Redis — Coding Plan (for pair-programming with an LLM)

Source material: *Build Your Own Redis with C/C++* (build-your-own.org/redis) — 14 chapters across 2 parts.
Reference for scope/testing ideas: CodeCrafters "Build Your Own Redis" challenge.

**Goal of this document:** a chapter-by-chapter build plan, each with a ready-to-paste prompt for an LLM
pair-programmer, plus an "interview polish" layer on top of the book so the finished repo reads as a
serious systems project, not just a tutorial clone.

---

## 0. How to use this file

1. Work through the parts **in order** — each one depends on the previous one's code.
2. For each part: paste that part's prompt into your LLM chat (a *fresh* one per part is fine, but paste
   in your current `server.c`/`server.cpp` and `client.c` first so the LLM has context).
3. Don't accept code you can't explain. After the LLM answers, ask it to explain the tricky lines
   (pointer arithmetic, the event loop state machine, the AVL rotations, etc.) before moving on.
4. Commit to git after each part (`git commit -m "Part N: <chapter title>"`). This alone is an interview
   asset — a commit history that mirrors a deliberate build-up is much more convincing than one squashed commit.
5. Track deviations from the book (bugs you hit, design choices you made differently) in `DECISIONS.md`.
   Interviewers care more about *why* you chose something than that you copied it correctly.

### Repo skeleton to create first

```
byo-redis/
  src/
    server.c(pp)
    client.c(pp)
    hashtable.c(pp)
    avl.c(pp)
    zset.c(pp)
    heap.c(pp)
    thread_pool.c(pp)
  tests/
  bench/
  docs/
    DECISIONS.md
    ARCHITECTURE.md
  Makefile / CMakeLists.txt
  Dockerfile
  README.md
```

---

## Part 1 — Foundations: Intro, Sockets, TCP Server/Client (Ch. 1–3)

**Book chapters:** 01 Introduction, 02 Socket Programming, 03 TCP Server and Client

**What you'll build:** a minimal TCP echo server and client using raw BSD sockets (`socket`, `bind`,
`listen`, `accept`, `connect`, `read`/`write`).

**Concepts to nail down before moving on:**
- Blocking vs non-blocking sockets
- `SO_REUSEADDR`, why it matters for restarting a dev server quickly
- Byte streams vs message framing (TCP has no message boundaries)
- Client/server roles and the connect/accept handshake

**Prompt to give the LLM:**
```
I'm building Redis from scratch in C++ following the book "Build Your Own Redis with C/C++"
(build-your-own.org/redis), chapters 1-3. Act as my pair-programmer and systems-programming
tutor for this part.

Goal: implement a minimal blocking TCP server and a matching client using raw POSIX sockets.

Requirements:
1. Walk me through the socket lifecycle (socket -> bind -> listen -> accept -> read/write -> close)
   with a short explanation of each syscall and its important flags/errors.
2. Give me a server.cpp that binds to 127.0.0.1:1234, accepts one connection at a time, reads
   whatever the client sends, and echoes it back.
3. Give me a matching client.cpp that connects, sends a message from argv, prints the reply.
4. Point out common beginner mistakes: not checking return values, not handling partial reads/writes,
   ignoring SIGPIPE, forgetting SO_REUSEADDR.
5. After the code, quiz me with 3 short questions about why TCP doesn't preserve message boundaries,
   so I can confirm I actually understood it before we move to protocol design.

Don't jump ahead to non-blocking IO or the event loop yet — that's the next part.
```

**Definition of done:** you can run the server, run the client from another terminal, and see the
echoed bytes; you can explain, without looking, why `read()` might return fewer bytes than you sent.

---

## Part 2 — Request/Response Protocol (Ch. 4)

**Book chapter:** 04 Request-Response Protocol

**What you'll build:** a length-prefixed message framing scheme on top of your TCP connection, so
the client can send discrete "requests" instead of raw bytes.

**Concepts:** message framing, `htonl`/`ntohl` and endianness, buffering partial reads, why
delimiter-based protocols (newline, etc.) break on binary data.

**Prompt:**
```
Continuing my from-scratch Redis build (Part 2 of the book, chapter 4: Request-Response Protocol).
Here is my current server.cpp/client.cpp: [paste code]

Goal: replace raw byte echoing with a length-prefixed request/response protocol:
  [4-byte little-endian length][payload bytes]

Requirements:
1. Explain why length-prefixing is the right choice vs delimiter-based framing, with a concrete
   example of how a delimiter scheme breaks.
2. Implement a `read_full` / `write_full` pair that loop until all requested bytes are
   transferred (handle partial read/write and EOF/error correctly).
3. Update client.cpp to send one length-prefixed message and read one length-prefixed reply.
4. Add a sane max-message-size limit and explain why an attacker-controlled length field is
   dangerous if unchecked.
5. Add a couple of test cases I can run by hand (short message, message right at the size limit,
   oversized message that should be rejected) and tell me the expected behavior for each.
```

**Definition of done:** client can send arbitrary binary payloads (not just printable text) and get
them back correctly; oversized/malformed length headers are rejected without crashing the server.

---

## Part 3 — Concurrency Models & the Event Loop (Ch. 5–6)

**Book chapters:** 05 Concurrent IO Models, 06 Event Loop (Part 1 & 2)

**What you'll build:** replace one-client-at-a-time blocking IO with a single-threaded event loop
using `poll()` (or `epoll` on Linux) so the server can handle many concurrent connections.

**Concepts:** thread-per-connection vs event loop vs thread pool trade-offs; non-blocking sockets
(`O_NONBLOCK`); `EAGAIN`/`EWOULDBLOCK`; per-connection state machine (reading vs writing vs closing);
level-triggered vs edge-triggered epoll.

**Prompt:**
```
Continuing my from-scratch Redis build, chapters 5-6: Concurrent IO Models and Event Loop.
Current code: [paste server.cpp]

Goal: rewrite the server around a single-threaded event loop so it can serve many clients
concurrently without one thread per connection.

Requirements:
1. First explain, compare-table style: blocking IO + thread-per-connection vs multiplexed IO
   (select/poll/epoll) vs io_uring — where each is used in real systems and why Redis chose
   the event-loop model historically.
2. Implement a `Conn` struct that tracks: fd, want_read/want_write/want_close flags, an incoming
   byte buffer, an outgoing byte buffer, and parsing state.
3. Implement the event loop using poll() (or epoll if I tell you I'm targeting Linux only):
   accept new connections, mark sockets non-blocking, drive reads/writes based on each Conn's
   buffers, and clean up closed connections without leaking fds.
4. Explain the difference between level-triggered and edge-triggered readiness notification and
   which one this code assumes.
5. Give me a way to stress-test this: e.g. a small script opening 50 concurrent connections and
   sending pipelined requests, so I can see it isn't serializing them behind a single blocking call.
6. Call out any resource-leak or infinite-loop pitfalls specific to non-blocking sockets (partial
   writes, forgetting to re-check want_write after a short write, etc.)
```

**Definition of done:** the server handles 50+ concurrent clients without spawning threads; you can
explain the `Conn` state machine diagrammatically; no fd leaks under repeated connect/disconnect
(verify with `lsof`).

---

## Part 4 — Key-Value Server (Ch. 7)

**Book chapter:** 07 Key-Value Server

**What you'll build:** wire up `GET`/`SET`/`DEL` commands over your framed protocol, backed for now
by a `std::map` placeholder (real hashtable comes in Part 5).

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 7: Key-Value Server. Current code: [paste]

Goal: implement the Redis request/response wire format and GET/SET/DEL commands.

Requirements:
1. Implement the request format: [nstr:u32][len:u32][str]...  — a list of strings, matching
   how a real command-line argv would look.
2. Implement a Response struct (status code + payload bytes) and the 3-step request handling
   pipeline: parse -> execute -> serialize.
3. Implement GET, SET, DEL against a temporary std::map<string,string> global store.
4. Explain explicitly *why* the book says "don't just use unordered_map and call it done" —
   what production requirements a real KV engine has that a bare STL container doesn't
   (incremental resizing, memory locality, no rehash pauses, custom hashing) — since I'll
   replace this with a real hashtable in the next part.
5. Give me an extra command as an exercise for me to implement myself (not you): e.g. EXISTS
   or MGET — then show me a checklist I can use to self-review my implementation before I show
   it back to you.
```

**Definition of done:** you can `SET foo bar`, `GET foo`, `DEL foo` via your client binary and get
correct status codes; you implemented at least one extra command yourself before moving on.

---

## Part 5 — Hashtables from Scratch (Ch. 8)

**Book chapters:** 08 Hashtables (Part 1 & 2)

**What you'll build:** your own open-addressing or chaining hashtable with **incremental resizing**
(the real trick — Redis never stalls to rehash the whole table at once), to replace `std::map`.

**Concepts:** hash function choice, chaining vs open addressing, load factor, incremental (progressive)
rehashing across two tables, amortized O(1) operations.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 8: Hashtables. Current code: [paste hashtable
stub / server.cpp using std::map]

Goal: implement a from-scratch hashtable with incremental resizing, matching the book's design
(chaining hashtable, two tables during rehash, a fixed amount of rehashing work done per
operation rather than all at once).

Requirements:
1. Explain incremental rehashing conceptually first: why stopping the world to resize a huge
   table is unacceptable in a low-latency server, and how doing "a few buckets per lookup"
   avoids that.
2. Implement the node structure (intrusive singly-linked list node embedded in the app's own
   struct, not a wrapping container — explain why the book uses intrusive data structures here).
3. Implement HTab (single fixed-size table: insert/lookup/remove by hash+compare callback) and
   HMap (the two-table wrapper managing progressive migration).
4. Wire GET/SET/DEL from Part 4 onto this new hashtable.
5. Give me a way to unit-test it in isolation (no networking): insert N random keys, trigger a
   resize, verify all keys are still found correctly during and after migration, verify deleted
   keys are gone from both tables.
6. Ask me to reason out loud about what load factor triggers your resize and why, before you
   confirm/correct my answer.
```

**Definition of done:** unit tests pass with resize happening live under load; you can explain, in
your own words, why intrusive linked lists avoid an extra allocation per hashtable node.

---

## Part 6 — Data Serialization (Ch. 9)

**Book chapter:** 09 Data Serialization

**What you'll build:** a proper serialization format for responses so you can return not just
strings but nil, integers, doubles, arrays, and errors — closer to the real RESP-like structure.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 9: Data Serialization. Current code: [paste]

Goal: extend the Response format to support multiple data types (nil, error, string, integer,
double, array of the above nested), similar in spirit to Redis's RESP protocol.

Requirements:
1. Define a tagged serialization format (1-byte type tag + type-specific payload) and explain
   the tradeoffs vs a text protocol like real RESP.
2. Implement serializer/deserializer helper functions for each type, plus a recursive array type.
3. Update do_request()/the command dispatch table so commands can return typed values
   (e.g. an error type for unknown commands, integer type for DEL's return count).
4. Update the client to pretty-print any response type recursively (so nested arrays print
   readably), which will also be my debugging tool going forward.
5. Show me how real Redis's RESP2/RESP3 differ from what we just built, as a comparison table,
   so I can talk about the difference in an interview if asked "how is this different from real
   Redis wire protocol".
```

**Definition of done:** client can pretty-print nested/typed responses; you can articulate 2–3
concrete differences between your format and RESP2/RESP3.

---

## Part 7 — Balanced Binary Tree / AVL (Ch. 10)

**Book chapter:** 10 Balanced Binary Tree (AVL)

**What you'll build:** an AVL tree from scratch (insert, delete, rotations, rebalancing) — the
building block for the sorted set in Part 8.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 10: AVL tree. This is pure data structures,
no networking involved yet.

Requirements:
1. Refresh me on the AVL invariant (balance factor <= 1) and why we need it (worst-case O(log n)
   guarantees vs an unbalanced BST degrading to O(n)).
2. Implement an intrusive AVL node (embedded in caller structs, like the hashtable), with
   left/right/parent pointers and a height or balance-factor field.
3. Implement insert with the 4 rotation cases (LL, RR, LR, RL), and delete with rebalancing on
   the way back up.
4. Implement in-order traversal and an "offset" query (find the node at rank k) since that's
   what we'll need for sorted-set range queries in the next part — explain how augmenting each
   node with subtree size enables O(log n) rank queries.
5. Give me a property-based test idea: insert/delete random sequences of N elements, and after
   every operation verify (a) the tree is still a valid BST, (b) the AVL balance property holds,
   (c) in-order traversal matches a reference sorted std::vector. Then give me the test harness.
6. Before showing we're "done", ask me to trace through one LR rotation case by hand and check
   my answer.
```

**Definition of done:** randomized insert/delete fuzz test passes for at least 10,000 operations
with invariant checks after each; you can draw and explain all 4 rotation cases from memory.

---

## Part 8 — Sorted Set (Ch. 11)

**Book chapter:** 11 Sorted Set

**What you'll build:** `ZADD`/`ZSCORE`/`ZRANGE`/`ZRANK`/`ZREM`-style commands backed by your AVL
tree (ordered by score) plus a hashtable (member -> score, for O(1) lookup by name).

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 11: Sorted Set. I have a working AVL tree and
hashtable from previous parts: [paste relevant code]

Goal: implement a ZSet combining both structures the way real Redis does: a hashtable for
O(1) member->score lookup, and an order-statistics tree (my AVL from Part 7, ordered by
(score, member) for tie-breaking) for O(log n) range/rank queries.

Requirements:
1. Explain why Redis uses this dual-structure design instead of just one of them.
2. Implement ZAdd (insert or update score - which means removing and re-inserting into the
   AVL if the score changes, explain why in-place score mutation isn't safe for a tree keyed
   by score).
3. Implement ZScore, ZRem, ZRank (using the offset/rank query from Part 7), and ZRange by rank
   (return members between rank lo and hi).
4. Wire these up as real commands (zadd, zscore, zrem, zrank, zquery) in the command dispatcher
   with correct RESP-like typed responses from Part 6.
5. Give me test cases covering: duplicate scores (tie-break by member name), updating an
   existing member's score, querying an empty/out-of-range rank window.
6. Ask me to explain, before confirming, why (score, member) needs to be the *combined* sort key
   rather than sorting by score alone.
```

**Definition of done:** you can add members with scores, query by rank range, and correctly handle
score ties, via your own client — this is usually the single most "wow" data-structure moment for
an interviewer, so be ready to whiteboard it.

---

## Part 9 — Timers and Timeouts (Ch. 12)

**Book chapter:** 12 Timer and Timeout

**What you'll build:** idle-connection timeouts inside the event loop, using a simple sorted
list/linked-list of connections ordered by last-active time.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 12: Timers and Timeouts. Current event loop:
[paste]

Goal: close idle connections automatically (e.g. after 5s of no activity) without a separate
thread, entirely inside the existing event loop.

Requirements:
1. Explain the design: an intrusive doubly-linked list of connections ordered by "last active
   time", so the connection needing to time out soonest is always at the head - O(1) to check.
2. Implement moving a connection to the tail of the list whenever it does IO.
3. Implement computing the poll()/epoll_wait() timeout dynamically as "time until the earliest
   possible timeout", instead of a fixed sleep, and explain why that's more efficient than
   polling every N ms.
4. Implement the timeout sweep: on each loop iteration, close any connections past the deadline.
5. Give me a manual test: open a connection, don't send anything, confirm the server closes it
   after ~5s, while other active connections stay open.
```

**Definition of done:** idle connections are reaped on schedule; active connections are
unaffected; CPU usage doesn't spike from busy-polling (you computed a dynamic timeout, not a
fixed short sleep).

---

## Part 10 — TTL / Cache Expiration with a Heap (Ch. 13)

**Book chapter:** 13 Cache Expiration with TTL

**What you'll build:** `EXPIRE`/`PEXPIRE`/`TTL`/`PERSIST`-style commands using a binary min-heap of
(expire-time, key) so expired keys are lazily *and* actively reclaimed.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 13: TTL with a heap. Current hashtable/server:
[paste]

Goal: implement key expiration using a min-heap keyed by absolute expiry timestamp, combined
with lazy expiration on access.

Requirements:
1. Explain the two expiration strategies real Redis uses (lazy: check on access; active: sample
   and evict in the background) and why relying on only one isn't enough.
2. Implement a binary min-heap (array-based) supporting: push, update-key-position (since a key's
   TTL can be refreshed), and pop-min, with each hashtable entry storing its own current heap
   index so updates are O(log n) not O(n) search.
3. Implement EXPIRE/TTL/PERSIST commands wired to the heap.
4. In the event loop, before computing poll's timeout, also account for "time until the next key
   expires" so the server wakes up in time to actively reclaim it even with no client traffic.
5. Add lazy-expiration checks into GET/SET/etc. so an expired-but-not-yet-reaped key is treated
   as absent.
6. Give me test cases: a key that expires while idle (active reclaim), a key checked exactly at
   its expiry boundary (lazy path), and TTL renewal via a second EXPIRE call updating heap
   position correctly.
```

**Definition of done:** keys disappear on schedule even with zero client traffic; you can explain
why storing a back-pointer/index inside the hashtable node avoids an O(n) heap search on update.

---

## Part 11 — Thread Pool for Blocking Work (Ch. 14)

**Book chapter:** 14 Thread Pool

**What you'll build:** an offload thread pool so that expensive operations (e.g. freeing a huge
value, or a large DEL) don't block the single event-loop thread.

**Prompt:**
```
Continuing my from-scratch Redis build, chapter 14: Thread Pool (final book chapter). Current
code: [paste]

Goal: add a small thread pool that the event loop can hand slow/blocking work off to, without
blocking the main loop thread.

Requirements:
1. Explain why a single-threaded event loop is vulnerable to one slow operation blocking every
   other client, and why real Redis added a background-thread mechanism (lazy-free) for exactly
   this reason.
2. Implement a simple thread pool: a task queue protected by mutex+condvar, N worker threads,
   and a way to submit a task (function pointer + arg).
3. Wire it up so that, e.g., destroying a very large value/collection is submitted to the pool
   instead of freed synchronously on the event-loop thread - explain the correctness care needed
   here (the value must be fully detached from shared state before handing to another thread).
4. Give me a synthetic test: create a huge value (e.g. 1M-member zset), DEL it, and show
   empirically that other clients' latency isn't affected by the delete (a before/after timing
   comparison script).
```

**Definition of done:** you have a reproducible before/after latency measurement showing the
thread pool actually fixes a real blocking problem — this artifact (a benchmark graph/table) is
excellent interview material.

---

## Part 12 — Beyond the Book: Interview-Differentiation Layer

The book stops at chapter 14. Everyone who reads it has roughly the same project. This part is
what turns "I followed a tutorial" into "I built a systems project" in an interviewer's eyes.
Pick 3–5 of these, don't try to do all of them — depth beats breadth here.

**Candidate extensions (roughly ordered by effort/impact):**

1. **Persistence (AOF-style)** — append every write command to a log file, replay it on startup.
   Talks well to "durability" and "crash recovery" interview questions.
2. **Pub/Sub** — `SUBSCRIBE`/`PUBLISH` commands; teaches you fan-out and connection-state beyond
   request/response.
3. **Transactions** — `MULTI`/`EXEC`/`DISCARD`, queuing commands per-connection then executing
   atomically inside the single-threaded loop (which is naturally atomic here — good talking point).
4. **LRU/LFU eviction policy** — bound memory usage and implement an eviction strategy when a
   max-memory limit is hit; compare policies.
5. **Simple leader-follower replication** — a second server instance that connects to the primary
   and replays its command stream. This is the single highest "wow" item for a systems-adjacent
   interview if you have time for only one.
6. **RESP2 wire-protocol compatibility** — implement real Redis's actual text protocol so
   `redis-cli` can talk to your server. Extremely convincing demo: `redis-cli -p 1234 set foo bar`
   just working against your own code.
7. **Benchmark suite** — a load generator (or reuse `redis-benchmark` if you did #6) producing
   throughput/latency numbers, plus a short write-up comparing your hashtable-with-incremental-
   resize against a naive `std::unordered_map` baseline under the same load. Concrete numbers you
   can quote in an interview are worth more than a feature checklist.

**Prompt template for each extension (fill in the bracket):**
```
Continuing my from-scratch Redis project, now going beyond the book. I have a working
event-loop server with hashtable, zset, TTL/heap, and thread pool from parts 1-11
(source pasted below).

Goal: implement [EXTENSION NAME] as an additional feature, at production-code quality, not a toy.

Requirements:
1. Before coding, give me a short design doc: what state needs to be added, what commands change,
   what failure modes exist, and how this interacts with the existing event loop / thread pool.
2. Implement it incrementally: propose the smallest working version first, then ask me whether
   I want to harden edge cases before adding more scope.
3. Point out at least one real-Redis design decision for this feature (e.g. how real Redis does
   replication/AOF/eviction) and how/why your simplified version differs.
4. Give me a test plan (manual + a script if feasible) that would convince a skeptical reviewer
   this actually works, including at least one failure/edge case (crash mid-write, subscriber
   disconnecting, evicting under memory pressure, etc.)
5. End with 2-3 interview-style questions an interviewer might ask about this feature, with
   model answers, so I can rehearse explaining it.
```

---

## Part 13 — Documentation, Testing & Presentation Polish

This is the part most people skip and the part that most affects how the project *reads* to an
interviewer skimming your GitHub for 90 seconds.

**Checklist:**
- [ ] `README.md` with: 1-paragraph pitch, architecture diagram (even ASCII), how to build/run,
      supported commands, what you built vs. the book vs. real Redis, benchmark numbers.
- [ ] `ARCHITECTURE.md` explaining the event loop, data structures, and design decisions —
      this is what you show if asked "walk me through your code."
- [ ] `DECISIONS.md` — a running log of non-obvious choices and trade-offs you made (great source
      of interview stories: "I initially did X, hit Y problem, switched to Z").
- [ ] Unit tests for hashtable, AVL, zset, heap (isolated from networking).
- [ ] An integration test script that starts the server and drives it through your client/redis-cli.
- [ ] A `Dockerfile` so anyone (including an interviewer) can `docker run` it in one command.
- [ ] Basic CI (GitHub Actions) running build + unit tests on push.
- [ ] A short demo — a GIF/asciinema recording or a `demo.sh` script that shows off the coolest
      parts (sorted set range query, TTL expiry, replication if you built it) in under 60 seconds.

**Prompt:**
```
I've finished implementing my from-scratch Redis (event loop, hashtable, AVL/zset, TTL heap,
thread pool, plus [list your Part 12 extensions]). Help me package this for a portfolio/interview
audience.

Requirements:
1. Draft a README.md: pitch paragraph, feature list, architecture summary with an ASCII diagram
   of the event loop and data flow, build/run instructions, and a "what I built vs. the book vs.
   real Redis" comparison table.
2. Draft ARCHITECTURE.md going one level deeper: the Conn state machine, how hashtable/AVL/heap
   are wired together, and where the thread pool sits relative to the event loop.
3. Write a GitHub Actions workflow that builds the project and runs my unit tests on every push.
4. Write a Dockerfile for the server.
5. Suggest 5 likely interview questions about this project (ranging from "why an event loop
   instead of threads" to "what would break under X failure") with strong model answers I can
   study, tailored to what I actually built.
```

---

## Appendix A — Source Code Reference

The book publishes full source for each chapter at `https://build-your-own.org/redis/src.tgz` and
per-chapter files linked at the bottom of each page (e.g. `07_server.cpp`). Use these to check your
own code against, but write your own version from the explanations first — pasting the book's code
into the LLM to "explain what this does" teaches you far less than writing it yourself and asking
the LLM to review/debug it.

## Appendix B — Using CodeCrafters alongside this plan

If you also have a CodeCrafters "Build Your Own Redis" subscription, it's most useful as a
**testing harness**, not a second source of instructions: CodeCrafters gives you an automated test
suite (stage by stage) that verifies real RESP-protocol compatibility. A good combo:
1. Build the *architecture* (event loop, hashtable, AVL, zset, TTL, thread pool) following Parts
   1–11 above, in C/C++, for the deep data-structures learning.
2. Optionally implement Appendix A's item "RESP2 wire-protocol compatibility" (Part 12, #6) so your
   server can pass CodeCrafters' test stages, which doubles as free, rigorous protocol-conformance
   testing you didn't have to write yourself.

## Appendix C — Suggested pacing

| Parts | Chapters | Rough effort |
|---|---|---|
| 1–4 | Sockets → KV server | 1 weekend |
| 5–6 | Hashtable, serialization | 1 weekend |
| 7–8 | AVL tree, sorted set | 1–2 weekends (hardest data-structure part) |
| 9–11 | Timers, TTL heap, thread pool | 1 weekend |
| 12 | 3–5 chosen extensions | 1–3 weekends depending on scope |
| 13 | Docs/tests/polish | 1–2 evenings |
