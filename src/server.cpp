// server.cpp — Part 6: Data serialization (Chapter 9)
//
// WHAT CHANGED FROM PART 5 (Hashtable):
//   The old response format was: [status:u32][raw bytes]
//   Three status codes (RES_OK, RES_ERR, RES_NX) with untyped string payloads.
//   This was limiting — DEL returned "1" as a string, EXISTS returned "0" as a
//   string, and there was no way to distinguish "nil" from an empty string.
//
//   The new format is: [type_tag:u8][type-specific payload]
//   Each response is a self-describing typed value (nil, error, string, int,
//   double, or recursively nested array). This is the same idea as Redis's
//   RESP protocol, but binary instead of text.
//
//   Benefits:
//   - DEL returns an actual integer (1 or 0), not a string "1" or "0"
//   - GET on a missing key returns SER_NIL, distinct from an empty string
//   - Errors carry both a code and a message
//   - Arrays enable KEYS, MGET, and future sorted-set range commands
//   - The client can pretty-print any type without guessing
//
// Wire format (unchanged outer frame):
//   [4 bytes: payload length, little-endian uint32] [payload bytes]
//   The payload is now a single serialized typed value (see serialization.h).

// Ensure WSAPoll and related constants are available (Windows Vista+)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <vector>
#include <string>
#include "hashtable.h"
#include "resp.h"
#include "zset.h"
#include "dlist.h"
#include "heap.h"
#include "thread_pool.h"
#include "aof.h"

// --------------------------------------------------------------------------
// WSAPoll compatibility shim for MinGW.
//
// Some MinGW distributions (especially older MinGW-w32) don't declare
// WSAPoll, WSAPOLLFD, or the POLL* constants even when _WIN32_WINNT >= 0x0600.
// We provide our own declarations here. These are ABI-compatible with the
// definitions in the Windows SDK — the function itself is in ws2_32.dll and
// links correctly via -lws2_32.
// --------------------------------------------------------------------------
#ifndef POLLRDNORM
#define POLLRDNORM  0x0100
#define POLLRDBAND  0x0200
#define POLLIN      (POLLRDNORM | POLLRDBAND)
#define POLLPRI     0x0400

#define POLLWRNORM  0x0010
#define POLLOUT     (POLLWRNORM)
#define POLLWRBAND  0x0020

#define POLLERR     0x0001
#define POLLHUP     0x0002
#define POLLNOVAL   0x0004

typedef struct pollfd {
    SOCKET fd;
    SHORT  events;
    SHORT  revents;
} WSAPOLLFD;

extern "C" {
    WINSOCK_API_LINKAGE int WSAAPI WSAPoll(
        WSAPOLLFD *fdArray, ULONG fds, INT timeout);
}
#endif

// --------------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------------
static const size_t k_max_msg = 4096;   // max payload bytes per message

// Error codes for SER_ERR responses.
// These are application-level codes embedded inside the error payload,
// NOT the old status enum. We define them as constants for consistency.
enum {
    ERR_UNKNOWN   = 1,   // unknown command
    ERR_BADARGS   = 2,   // wrong number of arguments
};

// --------------------------------------------------------------------------
// Global key-value store — backed by our custom HMap hashtable.
// --------------------------------------------------------------------------
enum {
    T_STR = 0,
    T_ZSET = 1,
};

struct Entry {
    HNode  node;           // intrusive hash node (must be a member, not a pointer)
    std::string key;
    uint32_t type = 0;
    std::string val;       // for T_STR
    ZSet *zset = NULL;     // for T_ZSET
    size_t heap_idx = (size_t)-1; // index in g_heap, or (size_t)-1 if no TTL
};

// --------------------------------------------------------------------------
// Thread Pool for slow / blocking operations (Chapter 14)
// --------------------------------------------------------------------------
static ThreadPool g_tp;
static const size_t k_large_container_size = 1000;
static bool g_lazyfree_enabled = true;

static void entry_destroy(Entry *ent) {
    if (ent->type == T_ZSET) {
        zset_destroy(ent->zset);
        delete ent->zset;
        ent->zset = NULL;
    }
}

// Background worker tasks for async deallocations
static void entry_del_async(void *arg) {
    Entry *ent = (Entry *)arg;
    entry_destroy(ent);
    delete ent;
}

static void zset_del_async(void *arg) {
    ZSet *zset = (ZSet *)arg;
    zset_destroy(zset);
    delete zset;
}

// FNV-1a hash
static uint64_t str_hash(const std::string &s) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;           // FNV prime
    }
    return h;
}

// Equality comparator for HMap operations.
static bool entry_eq(HNode *lhs, HNode *rhs) {
    Entry *le = container_of(lhs, Entry, node);
    Entry *re = container_of(rhs, Entry, node);
    return le->key == re->key;
}

// The global key-value store and TTL min-heap.
static HMap g_map = {};
static std::vector<HeapItem> g_heap;
AofState g_aof = {};

// Wall-clock time in milliseconds (Unix epoch) for absolute AOF timestamps
static uint64_t get_wall_clock_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// Monotonic clock reading in microseconds
static uint64_t get_monotonic_usec() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

// Detach an entry from hashtable and heap, then free it completely.
// If the collection is large and lazyfree is enabled, the expensive destruction
// is offloaded to the thread pool so the event loop is not blocked.
//
// CORRECTNESS CARE:
// 1. We MUST completely detach the entry from g_map (hm_delete) and g_heap
//    (heap_delete) BEFORE handing the pointer to the thread pool.
// 2. Once detached, no event loop operation can observe or modify 'ent'.
// 3. The worker thread has exclusive ownership of 'ent' and frees it safely.
static void detach_and_free_entry(Entry *ent) {
    hm_delete(&g_map, &ent->node, &entry_eq);
    if (ent->heap_idx != (size_t)-1) {
        heap_delete(g_heap, ent->heap_idx);
        ent->heap_idx = (size_t)-1;
    }

    bool too_big = false;
    if (g_lazyfree_enabled) {
        if (ent->type == T_ZSET && ent->zset) {
            too_big = hm_size(&ent->zset->hmap) > k_large_container_size;
        }
    }

    if (too_big) {
        thread_pool_queue(&g_tp, &entry_del_async, ent);
    } else {
        entry_destroy(ent);
        delete ent;
    }
}

// Look up a key in g_map and perform lazy expiration if it has expired.
// Returns Entry* if key exists and is alive; returns NULL if key doesn't exist or was expired.
static Entry *get_live_entry(const std::string &key_str) {
    Entry key;
    key.node.hcode = str_hash(key_str);
    key.key = key_str;

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    if (!node) {
        return NULL;
    }

    Entry *ent = container_of(node, Entry, node);
    if (ent->heap_idx != (size_t)-1) {
        uint64_t now_us = get_monotonic_usec();
        if (g_heap[ent->heap_idx].val <= now_us) {
            detach_and_free_entry(ent);
            return NULL;
        }
    }
    return ent;
}

// --------------------------------------------------------------------------
// Connection state enum.
// --------------------------------------------------------------------------
enum {
    STATE_READ = 0,
    STATE_WRITE = 1,
};

struct QueuedCmd {
    std::vector<std::string> argv;
};

static const size_t k_max_tx_commands = 10000;
static const size_t k_max_tx_bytes    = 16 * 1024 * 1024; // 16 MB

// --------------------------------------------------------------------------
// Conn — per-connection state.
// --------------------------------------------------------------------------
struct Conn {
    SOCKET   fd = INVALID_SOCKET;
    int      state = STATE_READ;    // STATE_READ or STATE_WRITE
    bool     want_close = false;   // marked for cleanup at end of event loop iteration

    // Read buffer (dynamic to support pipelining and large messages)
    std::string rbuf;

    // Write buffer
    std::string wbuf;
    size_t   wbuf_sent = 0;        // bytes already sent

    // Idle timer tracking
    uint64_t idle_start = 0;       // timestamp in microseconds of last IO activity
    DList    idle_list = {};       // intrusive doubly-linked list node in g_idle_list

    // Transaction state (MULTI / EXEC / DISCARD)
    bool in_multi = false;
    bool tx_abort = false;
    std::vector<QueuedCmd> tx_queue;
    size_t tx_queued_bytes = 0;
};

// --------------------------------------------------------------------------
// Idle timers and timeouts (Chapter 12)
// --------------------------------------------------------------------------
static const uint64_t k_idle_timeout_us = 5ULL * 1000 * 1000; // 5 seconds

// Global intrusive list of all active connections, ordered by idle_start
// with the oldest (soonest-to-expire) connection always at g_idle_list.next.
static DList g_idle_list = {};

// Update last active timestamp and move connection to the tail of g_idle_list
static void conn_touch(Conn *conn) {
    conn->idle_start = get_monotonic_usec();
    dlist_detach(&conn->idle_list);
    dlist_insert_before(&g_idle_list, &conn->idle_list);
}

// Compute dynamic poll timeout: milliseconds until the earliest timeout fires.
// Considers BOTH idle connections (g_idle_list) and key TTL expirations (g_heap).
static int next_timer_ms() {
    uint64_t now_us = get_monotonic_usec();
    uint64_t next_us = (uint64_t)-1;

    // Timer source 1: Idle connection timeout (head of intrusive list)
    if (!dlist_empty(&g_idle_list)) {
        Conn *conn = container_of(g_idle_list.next, Conn, idle_list);
        uint64_t conn_deadline = conn->idle_start + k_idle_timeout_us;
        if (conn_deadline < next_us) {
            next_us = conn_deadline;
        }
    }

    // Timer source 2: Key TTL expiration (root of binary min-heap)
    if (!g_heap.empty()) {
        uint64_t key_deadline = g_heap[0].val;
        if (key_deadline < next_us) {
            next_us = key_deadline;
        }
    }

    if (next_us == (uint64_t)-1) {
        return g_aof.rewrite_in_progress ? 50 : 10000;
    }

    if (next_us <= now_us) {
        return 0; // earliest deadline has passed; do not block in poll
    }

    // Convert microsecond delta to millisecond timeout, rounding up
    int res_ms = (int)((next_us - now_us + 999) / 1000);
    // If a background AOF rewrite is active, clamp poll timeout to 50ms so we check completion promptly
    if (g_aof.rewrite_in_progress && res_ms > 50) {
        res_ms = 50;
    }
    return res_ms;
}

// Timeout sweep: close idle connections and actively reclaim expired keys
static void process_timers() {
    uint64_t now_us = get_monotonic_usec();

    // 1. Idle connection sweep
    while (!dlist_empty(&g_idle_list)) {
        Conn *conn = container_of(g_idle_list.next, Conn, idle_list);
        uint64_t next_us = conn->idle_start + k_idle_timeout_us;
        if (next_us > now_us) {
            // Because connections are ordered monotonically by idle_start,
            // if the head has not timed out, no subsequent connection has either.
            break;
        }
        printf("Connection fd=%u timed out after %u ms idle.\n",
               (unsigned)conn->fd,
               (unsigned)((now_us - conn->idle_start) / 1000));
        conn->want_close = true;
        dlist_detach(&conn->idle_list);
    }

    // 2. Active TTL key expiration sweep (min-heap root eviction)
    while (!g_heap.empty() && g_heap[0].val <= now_us) {
        Entry *ent = container_of(g_heap[0].ref, Entry, heap_idx);
        detach_and_free_entry(ent);
    }
}

static void die(const char *msg) {
    fprintf(stderr, "[error] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

// --------------------------------------------------------------------------
// set_nonblocking — put a socket into non-blocking mode.
// --------------------------------------------------------------------------
static void set_nonblocking(SOCKET fd) {
    u_long mode = 1;   // 1 = enable non-blocking
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        die("ioctlsocket(FIONBIO)");
    }
}

// Forward declarations
static void state_read(Conn *conn);
static void state_write(Conn *conn);

// --------------------------------------------------------------------------
// Command handlers — serialize RESP2 responses directly to std::string.
// --------------------------------------------------------------------------

static void do_get(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'get' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_null_bulk(out);
    } else {
        if (ent->type != T_STR) {
            resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
            return;
        }
        resp_out_bulk(out, ent->val);
    }
}

static void do_set(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'set' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (ent) {
        if (ent->heap_idx != (size_t)-1) {
            heap_delete(g_heap, ent->heap_idx);
            ent->heap_idx = (size_t)-1;
        }
        if (ent->type == T_ZSET) {
            ZSet *zset = ent->zset;
            ent->zset = NULL;
            ent->type = T_STR;
            if (g_lazyfree_enabled && hm_size(&zset->hmap) > k_large_container_size) {
                thread_pool_queue(&g_tp, &zset_del_async, zset);
            } else {
                zset_destroy(zset);
                delete zset;
            }
        }
        ent->val = cmd[2];
    } else {
        ent = new Entry();
        ent->key = cmd[1];
        ent->type = T_STR;
        ent->val = cmd[2];
        ent->node.hcode = str_hash(cmd[1]);
        ent->heap_idx = (size_t)-1;
        hm_insert(&g_map, &ent->node);
    }

    resp_out_str(out, "OK");
    aof_append({"SET", cmd[1], cmd[2]});
}

static void do_del(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() < 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'del' command");
        return;
    }

    int64_t count = 0;
    std::vector<std::string> deleted_keys;
    for (size_t i = 1; i < cmd.size(); i++) {
        Entry *ent = get_live_entry(cmd[i]);
        if (ent) {
            detach_and_free_entry(ent);
            count++;
            deleted_keys.push_back(cmd[i]);
        }
    }
    if (count > 0) {
        std::vector<std::string> aof_del = {"DEL"};
        aof_del.insert(aof_del.end(), deleted_keys.begin(), deleted_keys.end());
        aof_append(aof_del);
    }
    resp_out_int(out, count);
}

static void do_exists(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() < 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'exists' command");
        return;
    }

    int64_t count = 0;
    for (size_t i = 1; i < cmd.size(); i++) {
        Entry *ent = get_live_entry(cmd[i]);
        if (ent) count++;
    }
    resp_out_int(out, count);
}

static void do_keys(const std::vector<std::string> &cmd, std::string &out) {
    (void)cmd;
    uint64_t now_us = get_monotonic_usec();
    std::vector<std::string> keys;

    auto collect_keys = [&](HTab *tab) {
        if (!tab || !tab->tab) return;
        for (size_t i = 0; i <= tab->mask; i++) {
            HNode *node = tab->tab[i];
            while (node) {
                Entry *ent = container_of(node, Entry, node);
                node = node->next;
                if (ent->heap_idx != (size_t)-1 && g_heap[ent->heap_idx].val <= now_us) {
                    continue; // Skip expired key
                }
                keys.push_back(ent->key);
            }
        }
    };

    collect_keys(&g_map.ht1);
    collect_keys(&g_map.ht2);

    resp_out_arr_header(out, keys.size());
    for (const std::string &k : keys) {
        resp_out_bulk(out, k);
    }
}

static void do_zadd(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 4) {
        resp_out_err(out, "ERR wrong number of arguments for 'zadd' command");
        return;
    }
    double score = 0;
    try {
        score = std::stod(cmd[2]);
    } catch (...) {
        resp_out_err(out, "ERR value is not a valid float");
        return;
    }
    
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        ent = new Entry();
        ent->key = cmd[1];
        ent->node.hcode = str_hash(cmd[1]);
        ent->type = T_ZSET;
        ent->heap_idx = (size_t)-1;
        ent->zset = new ZSet();
        zset_init(ent->zset);
        hm_insert(&g_map, &ent->node);
    } else {
        if (ent->type != T_ZSET) {
            resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
            return;
        }
    }
    
    ZNode *existing = zset_lookup(ent->zset, cmd[3].data(), cmd[3].size());
    double old_score = existing ? existing->score : 0;
    bool was_present = (existing != NULL);
    bool added = zset_add(ent->zset, cmd[3].data(), cmd[3].size(), score);
    if (added || (was_present && old_score != score)) {
        aof_append({"ZADD", cmd[1], cmd[2], cmd[3]});
    }
    resp_out_int(out, added ? 1 : 0);
}

static void do_zscore(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'zscore' command");
        return;
    }
    
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_null_bulk(out);
        return;
    }
    if (ent->type != T_ZSET) {
        resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    ZNode *zn = zset_lookup(ent->zset, cmd[2].data(), cmd[2].size());
    if (zn) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "%.17g", zn->score);
        resp_out_bulk(out, sbuf);
    } else {
        resp_out_null_bulk(out);
    }
}

static void do_zrem(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'zrem' command");
        return;
    }
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, 0);
        return;
    }
    if (ent->type != T_ZSET) {
        resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    ZNode *zn = zset_pop(ent->zset, cmd[2].data(), cmd[2].size());
    if (zn) {
        delete zn;
        aof_append({"ZREM", cmd[1], cmd[2]});
        resp_out_int(out, 1);
    } else {
        resp_out_int(out, 0);
    }
}

static void do_zrank(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'zrank' command");
        return;
    }
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_null_bulk(out);
        return;
    }
    if (ent->type != T_ZSET) {
        resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    int64_t rank = zset_rank(ent->zset, cmd[2].data(), cmd[2].size());
    if (rank >= 0) {
        resp_out_int(out, rank);
    } else {
        resp_out_null_bulk(out);
    }
}

static void do_zquery(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 6) {
        resp_out_err(out, "ERR wrong number of arguments for 'zquery' command");
        return;
    }
    double score = 0;
    int64_t offset = 0;
    int64_t limit = 0;
    try {
        score = std::stod(cmd[2]);
        offset = std::stoll(cmd[4]);
        limit = std::stoll(cmd[5]);
    } catch (...) {
        resp_out_err(out, "ERR value is not a valid float or integer");
        return;
    }
    
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_arr_header(out, 0);
        return;
    }
    if (ent->type != T_ZSET) {
        resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    std::vector<std::pair<std::string, double>> res;
    zset_query(ent->zset, score, cmd[3].data(), cmd[3].size(), offset, limit, res);
    
    resp_out_arr_header(out, res.size() * 2);
    for (const auto &p : res) {
        resp_out_bulk(out, p.first);
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "%.17g", p.second);
        resp_out_bulk(out, sbuf);
    }
}

static void do_expire(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'expire' command");
        return;
    }

    int64_t ttl_sec = 0;
    try {
        ttl_sec = std::stoll(cmd[2]);
    } catch (...) {
        resp_out_err(out, "ERR value is not an integer or out of range");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, 0);
        return;
    }

    if (ttl_sec <= 0) {
        detach_and_free_entry(ent);
        aof_append({"DEL", cmd[1]});
        resp_out_int(out, 1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = now_us + (uint64_t)ttl_sec * 1000000ULL;

    if (ent->heap_idx != (size_t)-1) {
        heap_update_val(g_heap, ent->heap_idx, expire_us);
    } else {
        heap_insert(g_heap, expire_us, &ent->heap_idx);
    }

    uint64_t now_wall_ms = get_wall_clock_ms();
    uint64_t expire_at_ms = now_wall_ms + (uint64_t)ttl_sec * 1000ULL;
    aof_append({"PEXPIREAT", cmd[1], std::to_string(expire_at_ms)});

    resp_out_int(out, 1);
}

static void do_pexpire(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'pexpire' command");
        return;
    }

    int64_t ttl_ms = 0;
    try {
        ttl_ms = std::stoll(cmd[2]);
    } catch (...) {
        resp_out_err(out, "ERR value is not an integer or out of range");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, 0);
        return;
    }

    if (ttl_ms <= 0) {
        detach_and_free_entry(ent);
        aof_append({"DEL", cmd[1]});
        resp_out_int(out, 1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = now_us + (uint64_t)ttl_ms * 1000ULL;

    if (ent->heap_idx != (size_t)-1) {
        heap_update_val(g_heap, ent->heap_idx, expire_us);
    } else {
        heap_insert(g_heap, expire_us, &ent->heap_idx);
    }

    uint64_t now_wall_ms = get_wall_clock_ms();
    uint64_t expire_at_ms = now_wall_ms + (uint64_t)ttl_ms;
    aof_append({"PEXPIREAT", cmd[1], std::to_string(expire_at_ms)});

    resp_out_int(out, 1);
}

static void do_ttl(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'ttl' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, -2);
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        resp_out_int(out, -1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = g_heap[ent->heap_idx].val;
    if (expire_us <= now_us) {
        detach_and_free_entry(ent);
        resp_out_int(out, -2);
        return;
    }

    int64_t remain_sec = (int64_t)((expire_us - now_us + 999999ULL) / 1000000ULL);
    resp_out_int(out, remain_sec);
}

static void do_pttl(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'pttl' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, -2);
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        resp_out_int(out, -1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = g_heap[ent->heap_idx].val;
    if (expire_us <= now_us) {
        detach_and_free_entry(ent);
        resp_out_int(out, -2);
        return;
    }

    int64_t remain_ms = (int64_t)((expire_us - now_us + 999ULL) / 1000ULL);
    resp_out_int(out, remain_ms);
}

static void do_pexpireat(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'pexpireat' command");
        return;
    }

    uint64_t expire_at_ms = 0;
    try {
        expire_at_ms = std::stoull(cmd[2]);
    } catch (...) {
        resp_out_err(out, "ERR value is not an integer or out of range");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, 0);
        return;
    }

    uint64_t now_ms = get_wall_clock_ms();
    if (expire_at_ms <= now_ms) {
        detach_and_free_entry(ent);
        aof_append({"DEL", cmd[1]});
        resp_out_int(out, 1);
        return;
    }

    uint64_t remaining_ms = expire_at_ms - now_ms;
    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = now_us + remaining_ms * 1000ULL;

    if (ent->heap_idx != (size_t)-1) {
        heap_update_val(g_heap, ent->heap_idx, expire_us);
    } else {
        heap_insert(g_heap, expire_us, &ent->heap_idx);
    }

    aof_append({"PEXPIREAT", cmd[1], cmd[2]});
    resp_out_int(out, 1);
}

static void do_persist(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'persist' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        resp_out_int(out, 0);
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        resp_out_int(out, 0);
        return;
    }

    heap_delete(g_heap, ent->heap_idx);
    ent->heap_idx = (size_t)-1;
    aof_append({"PERSIST", cmd[1]});
    resp_out_int(out, 1);
}

static void do_ping(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() == 1) {
        resp_out_str(out, "PONG");
    } else if (cmd.size() == 2) {
        resp_out_bulk(out, cmd[1]);
    } else {
        resp_out_err(out, "ERR wrong number of arguments for 'ping' command");
    }
}

static void do_echo(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 2) {
        resp_out_err(out, "ERR wrong number of arguments for 'echo' command");
    } else {
        resp_out_bulk(out, cmd[1]);
    }
}

static void do_config(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() >= 3 && (cmd[1] == "set" || cmd[1] == "SET")) {
        if (cmd[2] == "lazyfree" || cmd[2] == "LAZYFREE") {
            if (cmd.size() < 4) {
                resp_out_err(out, "ERR wrong number of arguments for 'config set'");
                return;
            }
            if (cmd[3] == "1" || cmd[3] == "yes" || cmd[3] == "true") {
                g_lazyfree_enabled = true;
                resp_out_str(out, "OK");
            } else if (cmd[3] == "0" || cmd[3] == "no" || cmd[3] == "false") {
                g_lazyfree_enabled = false;
                resp_out_str(out, "OK");
            } else {
                resp_out_err(out, "ERR value must be 0 or 1");
            }
            return;
        }
    } else if (cmd.size() >= 3 && (cmd[1] == "get" || cmd[1] == "GET")) {
        if (cmd[2] == "lazyfree" || cmd[2] == "LAZYFREE") {
            resp_out_arr_header(out, 2);
            resp_out_bulk(out, "lazyfree");
            resp_out_bulk(out, g_lazyfree_enabled ? "yes" : "no");
            return;
        }
    }
    resp_out_err(out, "ERR unsupported config command");
}

static void do_zpopulate(const std::vector<std::string> &cmd, std::string &out) {
    if (cmd.size() != 3) {
        resp_out_err(out, "ERR wrong number of arguments for 'zpopulate' command");
        return;
    }
    int64_t count = 0;
    try {
        count = std::stoll(cmd[2]);
    } catch (...) {
        resp_out_err(out, "ERR invalid count");
        return;
    }
    if (count <= 0) {
        resp_out_int(out, 0);
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        ent = new Entry();
        ent->key = cmd[1];
        ent->node.hcode = str_hash(cmd[1]);
        ent->type = T_ZSET;
        ent->heap_idx = (size_t)-1;
        ent->zset = new ZSet();
        zset_init(ent->zset);
        hm_insert(&g_map, &ent->node);
    } else if (ent->type != T_ZSET) {
        resp_out_err(out, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }

    int64_t added = 0;
    for (int64_t i = 0; i < count; i++) {
        std::string name = "m" + std::to_string(i);
        if (zset_add(ent->zset, name.data(), name.size(), (double)i)) {
            added++;
        }
    }
    resp_out_int(out, added);
}

// --------------------------------------------------------------------------
// AOF Compaction / Snapshot Generation
// --------------------------------------------------------------------------
static std::string aof_generate_snapshot_data() {
    uint64_t now_us = get_monotonic_usec();
    uint64_t now_wall_ms = get_wall_clock_ms();
    std::string out;

    auto dump_entry = [&](Entry *ent) {
        // Skip expired keys
        if (ent->heap_idx != (size_t)-1 && g_heap[ent->heap_idx].val <= now_us) {
            return;
        }

        // 1. Recreate value (String or ZSet)
        if (ent->type == T_STR) {
            std::vector<std::string> set_cmd = {"SET", ent->key, ent->val};
            resp_out_arr_header(out, set_cmd.size());
            for (const auto &arg : set_cmd) {
                resp_out_bulk(out, arg);
            }
        } else if (ent->type == T_ZSET && ent->zset) {
            auto dump_zset_tab = [&](HTab *tab) {
                if (!tab || !tab->tab) return;
                for (size_t i = 0; i <= tab->mask; i++) {
                    HNode *node = tab->tab[i];
                    while (node) {
                        ZNode *zn = container_of(node, ZNode, hmap);
                        node = node->next;
                        char sbuf[64];
                        snprintf(sbuf, sizeof(sbuf), "%.17g", zn->score);
                        std::vector<std::string> zadd_cmd = {"ZADD", ent->key, sbuf, zn->name};
                        resp_out_arr_header(out, zadd_cmd.size());
                        for (const auto &arg : zadd_cmd) {
                            resp_out_bulk(out, arg);
                        }
                    }
                }
            };
            dump_zset_tab(&ent->zset->hmap.ht1);
            dump_zset_tab(&ent->zset->hmap.ht2);
        }

        // 2. Recreate TTL if key has unexpired TTL
        if (ent->heap_idx != (size_t)-1) {
            uint64_t expire_us = g_heap[ent->heap_idx].val;
            if (expire_us > now_us) {
                uint64_t remain_ms = (expire_us - now_us) / 1000ULL;
                uint64_t expire_at_ms = now_wall_ms + remain_ms;
                std::vector<std::string> exp_cmd = {"PEXPIREAT", ent->key, std::to_string(expire_at_ms)};
                resp_out_arr_header(out, exp_cmd.size());
                for (const auto &arg : exp_cmd) {
                    resp_out_bulk(out, arg);
                }
            }
        }
    };

    auto scan_tab = [&](HTab *tab) {
        if (!tab || !tab->tab) return;
        for (size_t i = 0; i <= tab->mask; i++) {
            HNode *node = tab->tab[i];
            while (node) {
                Entry *ent = container_of(node, Entry, node);
                node = node->next;
                dump_entry(ent);
            }
        }
    };

    scan_tab(&g_map.ht1);
    scan_tab(&g_map.ht2);

    return out;
}

static void do_bgrewriteaof(const std::vector<std::string> &cmd, std::string &out) {
    (void)cmd;
    if (g_aof.rewrite_in_progress) {
        resp_out_err(out, "ERR Background append only file rewriting already in progress");
        return;
    }

    std::string snapshot = aof_generate_snapshot_data();
    std::string temp_file = "temp-" + g_aof.filename;

    if (!aof_start_bgrewrite(&g_tp, snapshot, temp_file)) {
        resp_out_err(out, "ERR failed to start background AOF rewrite");
        return;
    }

    resp_out_str(out, "Background append only file rewriting started");
}

static void do_rewriteaof(const std::vector<std::string> &cmd, std::string &out) {
    (void)cmd;
    if (g_aof.rewrite_in_progress) {
        resp_out_err(out, "ERR Background append only file rewriting already in progress");
        return;
    }

    std::string snapshot = aof_generate_snapshot_data();
    std::string temp_file = "temp-" + g_aof.filename;

    if (!aof_rewrite_sync(snapshot, temp_file)) {
        resp_out_err(out, "ERR synchronous AOF rewrite failed");
        return;
    }

    resp_out_str(out, "OK");
}

// Forward declaration for recursive execution in EXEC
static void do_request(Conn *conn, const std::vector<std::string> &cmd, std::string &out, bool &want_close);

// Syntax & arity validation for queue-time error detection (EXECABORT)
static bool validate_cmd_syntax(const std::vector<std::string> &cmd, std::string &err_msg) {
    if (cmd.empty()) {
        err_msg = "ERR unknown command ''";
        return false;
    }

    std::string c = cmd[0];
    for (char &ch : c) ch = (char)toupper((unsigned char)ch);

    if (c == "PING") {
        if (cmd.size() > 2) {
            err_msg = "ERR wrong number of arguments for 'ping' command";
            return false;
        }
        return true;
    }
    if (c == "ECHO") {
        if (cmd.size() != 2) {
            err_msg = "ERR wrong number of arguments for 'echo' command";
            return false;
        }
        return true;
    }
    if (c == "GET") {
        if (cmd.size() != 2) {
            err_msg = "ERR wrong number of arguments for 'get' command";
            return false;
        }
        return true;
    }
    if (c == "SET") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'set' command";
            return false;
        }
        return true;
    }
    if (c == "DEL" || c == "UNLINK") {
        if (cmd.size() < 2) {
            err_msg = "ERR wrong number of arguments for 'del' command";
            return false;
        }
        return true;
    }
    if (c == "EXISTS") {
        if (cmd.size() < 2) {
            err_msg = "ERR wrong number of arguments for 'exists' command";
            return false;
        }
        return true;
    }
    if (c == "KEYS") {
        return true;
    }
    if (c == "EXPIRE") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'expire' command";
            return false;
        }
        return true;
    }
    if (c == "PEXPIRE") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'pexpire' command";
            return false;
        }
        return true;
    }
    if (c == "PEXPIREAT") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'pexpireat' command";
            return false;
        }
        return true;
    }
    if (c == "TTL") {
        if (cmd.size() != 2) {
            err_msg = "ERR wrong number of arguments for 'ttl' command";
            return false;
        }
        return true;
    }
    if (c == "PTTL") {
        if (cmd.size() != 2) {
            err_msg = "ERR wrong number of arguments for 'pttl' command";
            return false;
        }
        return true;
    }
    if (c == "PERSIST") {
        if (cmd.size() != 2) {
            err_msg = "ERR wrong number of arguments for 'persist' command";
            return false;
        }
        return true;
    }
    if (c == "ZADD") {
        if (cmd.size() != 4) {
            err_msg = "ERR wrong number of arguments for 'zadd' command";
            return false;
        }
        return true;
    }
    if (c == "ZSCORE") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'zscore' command";
            return false;
        }
        return true;
    }
    if (c == "ZREM") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'zrem' command";
            return false;
        }
        return true;
    }
    if (c == "ZRANK") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'zrank' command";
            return false;
        }
        return true;
    }
    if (c == "ZQUERY") {
        if (cmd.size() != 6) {
            err_msg = "ERR wrong number of arguments for 'zquery' command";
            return false;
        }
        return true;
    }
    if (c == "ZPOPULATE") {
        if (cmd.size() != 3) {
            err_msg = "ERR wrong number of arguments for 'zpopulate' command";
            return false;
        }
        return true;
    }
    if (c == "CONFIG" || c == "CLIENT" || c == "COMMAND" || c == "HELLO" || c == "QUIT" || c == "BGREWRITEAOF" || c == "REWRITEAOF") {
        return true;
    }

    err_msg = "ERR unknown command '" + cmd[0] + "'";
    return false;
}

static void do_exec(Conn *conn, std::string &out) {
    if (!conn->in_multi) {
        resp_out_err(out, "ERR EXEC without MULTI");
        return;
    }

    if (conn->tx_abort) {
        conn->in_multi = false;
        conn->tx_abort = false;
        conn->tx_queue.clear();
        conn->tx_queued_bytes = 0;
        resp_out_err(out, "EXECABORT Transaction discarded because of previous errors.");
        return;
    }

    if (conn->tx_queue.empty()) {
        conn->in_multi = false;
        conn->tx_abort = false;
        resp_out_arr_header(out, 0); // *0\r\n
        return;
    }

    // Move out queue and exit multi mode before executing
    std::vector<QueuedCmd> queue = std::move(conn->tx_queue);
    conn->in_multi = false;
    conn->tx_abort = false;
    conn->tx_queued_bytes = 0;

    // Begin AOF transaction buffering
    aof_tx_begin();

    std::vector<std::string> results;
    results.reserve(queue.size());

    for (const auto &qcmd : queue) {
        std::string cmd_out;
        bool dummy_close = false;
        do_request(NULL, qcmd.argv, cmd_out, dummy_close);
        results.push_back(cmd_out);
    }

    // Commit atomic transaction block to AOF
    aof_tx_commit();

    // Assemble array of results: *<count>\r\n<r1><r2>...
    resp_out_arr_header(out, results.size());
    for (const auto &res : results) {
        out.append(res);
    }
}

// --------------------------------------------------------------------------
// Command dispatch — the central router.
// --------------------------------------------------------------------------
static void do_request(Conn *conn, const std::vector<std::string> &cmd, std::string &out, bool &want_close) {
    if (cmd.empty()) {
        return;
    }

    std::string c = cmd[0];
    for (char &ch : c) ch = (char)toupper((unsigned char)ch);

    // ----------------------------------------------------------------------
    // Transaction Mode: Queuing
    // ----------------------------------------------------------------------
    if (conn && conn->in_multi) {
        if (c == "MULTI") {
            resp_out_err(out, "ERR MULTI calls can not be nested");
            return;
        }
        if (c == "DISCARD") {
            conn->in_multi = false;
            conn->tx_abort = false;
            conn->tx_queue.clear();
            conn->tx_queued_bytes = 0;
            resp_out_str(out, "OK");
            return;
        }
        if (c == "EXEC") {
            do_exec(conn, out);
            return;
        }
        if (c == "QUIT") {
            resp_out_str(out, "OK");
            want_close = true;
            return;
        }

        // Validate syntax at queue time
        std::string err_msg;
        if (!validate_cmd_syntax(cmd, err_msg)) {
            conn->tx_abort = true;
            resp_out_err(out, err_msg);
            return;
        }

        size_t cmd_bytes = 0;
        for (const auto &s : cmd) cmd_bytes += s.size() + sizeof(std::string);
        if (conn->tx_queue.size() >= k_max_tx_commands || conn->tx_queued_bytes + cmd_bytes > k_max_tx_bytes) {
            conn->tx_abort = true;
            resp_out_err(out, "ERR transaction queue limit exceeded");
            return;
        }

        conn->tx_queue.push_back({cmd});
        conn->tx_queued_bytes += cmd_bytes;
        resp_out_str(out, "QUEUED");
        return;
    }

    // Top-level MULTI / EXEC / DISCARD
    if (c == "MULTI") {
        if (conn) {
            conn->in_multi = true;
            conn->tx_abort = false;
            conn->tx_queue.clear();
            conn->tx_queued_bytes = 0;
        }
        resp_out_str(out, "OK");
        return;
    }
    if (c == "EXEC") {
        resp_out_err(out, "ERR EXEC without MULTI");
        return;
    }
    if (c == "DISCARD") {
        resp_out_err(out, "ERR DISCARD without MULTI");
        return;
    }

    if (c == "PING") {
        do_ping(cmd, out);
    } else if (c == "ECHO") {
        do_echo(cmd, out);
    } else if (c == "QUIT") {
        resp_out_str(out, "OK");
        want_close = true;
    } else if (c == "COMMAND") {
        // Modern redis-cli sends COMMAND / COMMAND DOCS on connect
        resp_out_arr_header(out, 0); // Empty array: *0\r\n
    } else if (c == "HELLO") {
        // Fall back gracefully to RESP2
        resp_out_err(out, "ERR unknown command 'HELLO'");
    } else if (c == "CLIENT") {
        resp_out_str(out, "OK");
    } else if (c == "GET") {
        do_get(cmd, out);
    } else if (c == "SET") {
        do_set(cmd, out);
    } else if (c == "DEL" || c == "UNLINK") {
        do_del(cmd, out);
    } else if (c == "CONFIG") {
        do_config(cmd, out);
    } else if (c == "ZPOPULATE") {
        do_zpopulate(cmd, out);
    } else if (c == "EXISTS") {
        do_exists(cmd, out);
    } else if (c == "KEYS") {
        do_keys(cmd, out);
    } else if (c == "EXPIRE") {
        do_expire(cmd, out);
    } else if (c == "PEXPIRE") {
        do_pexpire(cmd, out);
    } else if (c == "TTL") {
        do_ttl(cmd, out);
    } else if (c == "PTTL") {
        do_pttl(cmd, out);
    } else if (c == "PERSIST") {
        do_persist(cmd, out);
    } else if (c == "PEXPIREAT") {
        do_pexpireat(cmd, out);
    } else if (c == "ZADD") {
        do_zadd(cmd, out);
    } else if (c == "ZSCORE") {
        do_zscore(cmd, out);
    } else if (c == "ZREM") {
        do_zrem(cmd, out);
    } else if (c == "ZRANK") {
        do_zrank(cmd, out);
    } else if (c == "ZQUERY") {
        do_zquery(cmd, out);
    } else if (c == "BGREWRITEAOF") {
        do_bgrewriteaof(cmd, out);
    } else if (c == "REWRITEAOF") {
        do_rewriteaof(cmd, out);
    } else {
        resp_out_err(out, "ERR unknown command '" + cmd[0] + "'");
    }
}

// --------------------------------------------------------------------------
// Request processing & Connection IO
// --------------------------------------------------------------------------
static void process_rbuf(Conn *conn) {
    size_t rbuf_idx = 0;
    while (rbuf_idx < conn->rbuf.size()) {
        size_t consumed = 0;
        std::vector<std::string> cmd;
        ParseStatus st = parse_request(
            (const uint8_t *)conn->rbuf.data() + rbuf_idx,
            conn->rbuf.size() - rbuf_idx,
            consumed,
            cmd
        );
        if (st == PARSE_NEED_MORE) {
            break;
        }
        if (st == PARSE_ERR) {
            fprintf(stderr, "Protocol error on fd %u, closing connection.\n", (unsigned)conn->fd);
            conn->want_close = true;
            break;
        }
        rbuf_idx += consumed;
        if (!cmd.empty()) {
            do_request(conn, cmd, conn->wbuf, conn->want_close);
            if (conn->want_close) break;
        }
    }
    if (rbuf_idx > 0) {
        conn->rbuf.erase(0, rbuf_idx);
    }
}

static void state_read(Conn *conn) {
    char buf[64 * 1024];
    int rv = recv(conn->fd, buf, sizeof(buf), 0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return;
        }
        fprintf(stderr, "recv() error on fd %u: %d\n", (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        printf("Client disconnected (fd=%u).\n", (unsigned)conn->fd);
        conn->want_close = true;
        return;
    }

    conn->rbuf.append(buf, (size_t)rv);
    conn_touch(conn);

    process_rbuf(conn);

    if (!conn->wbuf.empty()) {
        conn->state = STATE_WRITE;
        state_write(conn);
    }
}

static void state_write(Conn *conn) {
    if (conn->wbuf_sent >= conn->wbuf.size()) {
        conn->wbuf.clear();
        conn->wbuf_sent = 0;
        conn->state = STATE_READ;
        return;
    }

    int rv = send(conn->fd,
                  conn->wbuf.data() + conn->wbuf_sent,
                  (int)(conn->wbuf.size() - conn->wbuf_sent),
                  0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return;
        }
        fprintf(stderr, "send() error on fd %u: %d\n", (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    conn->wbuf_sent += (size_t)rv;
    conn_touch(conn);

    if (conn->wbuf_sent == conn->wbuf.size()) {
        conn->wbuf.clear();
        conn->wbuf_sent = 0;
        conn->state = STATE_READ;

        if (conn->want_close) {
            return;
        }

        if (!conn->rbuf.empty()) {
            process_rbuf(conn);
            if (!conn->wbuf.empty()) {
                conn->state = STATE_WRITE;
            }
        }
    }
}

// --------------------------------------------------------------------------
// conn_accept — accept all pending connections from the listener.
// --------------------------------------------------------------------------
static void conn_accept(SOCKET listen_fd, std::vector<Conn *> &connections) {
    while (true) {
        struct sockaddr_in client_addr = {};
        int addrlen = sizeof(client_addr);

        SOCKET conn_fd = accept(listen_fd,
                                (struct sockaddr *)&client_addr, &addrlen);
        if (conn_fd == INVALID_SOCKET) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                break;   // no more pending connections
            }
            fprintf(stderr, "accept() error: %d\n", err);
            break;
        }

        set_nonblocking(conn_fd);

        Conn *conn = new Conn();
        conn->fd = conn_fd;
        conn->state = STATE_READ;
        conn->want_close = false;
        conn->wbuf_sent = 0;
        dlist_init(&conn->idle_list);
        conn_touch(conn);

        connections.push_back(conn);
        printf("Client connected (fd=%u, total=%u).\n",
               (unsigned)conn_fd, (unsigned)connections.size());
    }
}

// ==========================================================================
// MAIN — the event loop
// ==========================================================================
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    std::string aof_filename = "appendonly.aof";
    int port = 1234;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--aof") == 0 && i + 1 < argc) {
            aof_filename = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    // --- Winsock init ---
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == INVALID_SOCKET) {
        die("socket()");
    }

    int val = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&val, sizeof(val)) == SOCKET_ERROR) {
        die("setsockopt(SO_REUSEADDR)");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("bind()");
    }

    if (listen(listen_fd, SOMAXCONN) == SOCKET_ERROR) {
        die("listen()");
    }

    set_nonblocking(listen_fd);

    // Initialize background thread pool with 4 workers for async deallocations
    thread_pool_init(&g_tp, 4);

    dlist_init(&g_idle_list);

    // Replay existing AOF state before serving live client traffic
    aof_load(aof_filename, [](const std::vector<std::string> &cmd) {
        std::string dummy_out;
        bool dummy_close = false;
        do_request(NULL, cmd, dummy_out, dummy_close);
    });

    // Open AOF in append mode
    if (!aof_init(aof_filename, AOF_FSYNC_ALWAYS)) {
        fprintf(stderr, "[aof] Failed to initialize AOF '%s'\n", aof_filename.c_str());
    }

    printf("Server listening on 127.0.0.1:%d (AOF: %s, always-fsync) ...\n",
           port, aof_filename.c_str());

    std::vector<Conn *> connections;

    // ======================================================================
    // THE EVENT LOOP
    // ======================================================================
    while (true) {
        // ---- Step 1: Build the poll set ----
        std::vector<WSAPOLLFD> pfds;

        {
            WSAPOLLFD pfd = {};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            pfds.push_back(pfd);
        }

        for (Conn *conn : connections) {
            WSAPOLLFD pfd = {};
            pfd.fd = conn->fd;

            if (conn->state == STATE_READ) {
                pfd.events = POLLIN;
            } else {
                pfd.events = POLLOUT;
            }
            pfds.push_back(pfd);
        }

        // ---- Step 2: Compute dynamic timeout and WSAPoll ----
        int timeout_ms = next_timer_ms();
        int rv = WSAPoll(pfds.data(), (ULONG)pfds.size(), timeout_ms);
        if (rv == SOCKET_ERROR) {
            die("WSAPoll()");
        }

        // ---- Step 3: Accept new connections ----
        if (pfds[0].revents & POLLIN) {
            conn_accept(listen_fd, connections);
        }

        // ---- Step 4: Drive IO on each connection ----
        for (size_t i = 0; i < connections.size(); i++) {
            if (pfds[i + 1].revents == 0) {
                continue;
            }

            bool handled = false;
            if (connections[i]->state == STATE_READ && (pfds[i + 1].revents & (POLLIN | POLLERR | POLLHUP))) {
                state_read(connections[i]);
                handled = true;
            } else if (connections[i]->state == STATE_WRITE && (pfds[i + 1].revents & (POLLOUT | POLLERR | POLLHUP))) {
                state_write(connections[i]);
                handled = true;
            }

            if (!handled && (pfds[i + 1].revents & (POLLERR | POLLHUP))) {
                fprintf(stderr, "unhandled poll error on fd %u: revents=%d\n", (unsigned)connections[i]->fd, pfds[i+1].revents);
                connections[i]->want_close = true;
            }
        }

        // ---- Step 5: Process timers (timeout sweep) ----
        process_timers();

        // ---- Check background AOF rewrite completion ----
        aof_check_rewrite_done();

        // ---- Step 6: Sweep closed connections ----
        for (int i = (int)connections.size() - 1; i >= 0; i--) {
            if (connections[i]->want_close) {
                Conn *conn = connections[i];
                printf("Closing connection fd=%u.\n",
                       (unsigned)conn->fd);
                dlist_detach(&conn->idle_list);
                closesocket(conn->fd);

                connections[i] = connections.back();
                connections.pop_back();
                delete conn;
            }
        }
    }

    // Cleanup — unreachable in the infinite loop, but good practice
    closesocket(listen_fd);
    WSACleanup();
    return 0;
}
