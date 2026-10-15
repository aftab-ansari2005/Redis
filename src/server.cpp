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
#include "serialization.h"
#include "zset.h"
#include "dlist.h"
#include "heap.h"
#include "thread_pool.h"

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

// --------------------------------------------------------------------------
// Conn — per-connection state.
// --------------------------------------------------------------------------
struct Conn {
    SOCKET   fd;
    int      state;         // STATE_READ or STATE_WRITE
    bool     want_close;    // marked for cleanup at end of event loop iteration

    // Read buffer
    uint8_t  rbuf[4 + k_max_msg];
    size_t   rbuf_size;     // valid bytes currently in rbuf

    // Write buffer
    uint8_t  wbuf[4 + k_max_msg];
    size_t   wbuf_size;     // total bytes in wbuf that need to be sent
    size_t   wbuf_sent;     // bytes already sent

    // Idle timer tracking
    uint64_t idle_start;    // timestamp in microseconds of last IO activity
    DList    idle_list;     // intrusive doubly-linked list node in g_idle_list
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
        return 10000; // default wait when no timers are active
    }

    if (next_us <= now_us) {
        return 0; // earliest deadline has passed; do not block in poll
    }

    // Convert microsecond delta to millisecond timeout, rounding up
    return (int)((next_us - now_us + 999) / 1000);
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
// Request parsing (unchanged from Part 5)
// --------------------------------------------------------------------------
static bool parse_req(const uint8_t *data, size_t len, std::vector<std::string> &out) {
    if (len < 4) return false;
    uint32_t nstr = 0;
    memcpy(&nstr, &data[0], 4);

    // Sane limit on arg count
    if (nstr > 1000) return false;

    size_t pos = 4;
    while (nstr--) {
        if (pos + 4 > len) return false;
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) return false;

        out.push_back(std::string((char*)&data[pos + 4], sz));
        pos += 4 + sz;
    }

    if (pos != len) return false;  // trailing garbage
    return true;
}

// --------------------------------------------------------------------------
// Command handlers — now serialize typed responses directly.
//
// BEFORE (Part 5):
//   void do_get(..., uint32_t &status, std::vector<uint8_t> &res);
//   Status was a flat enum, res was raw bytes.
//
// AFTER (Part 6):
//   void do_get(..., std::vector<uint8_t> &out);
//   The output IS the serialized typed value. No separate status field.
//   The type tag itself carries the "status" information:
//     - SER_NIL  = key not found (was RES_NX)
//     - SER_ERR  = error (was RES_ERR)
//     - SER_STR  = success with string (was RES_OK + bytes)
//     - SER_INT  = success with integer (new! DEL/EXISTS return int now)
// --------------------------------------------------------------------------

static void do_get(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'get' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        // Key not found or expired — return NIL
        out_nil(out);
    } else {
        if (ent->type != T_STR) {
            out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
            return;
        }
        out_str(out, ent->val);
    }
}

static void do_set(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'set' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (ent) {
        // Key exists and is alive — update value in place.
        // In Redis, SET clears any existing TTL unless KEEPTTL is specified.
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
        // Key doesn't exist (or was expired and lazily reaped)
        ent = new Entry();
        ent->key = cmd[1];
        ent->type = T_STR;
        ent->val = cmd[2];
        ent->node.hcode = str_hash(cmd[1]);
        ent->heap_idx = (size_t)-1;
        hm_insert(&g_map, &ent->node);
    }

    out_str(out, "OK");
}

static void do_del(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'del' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        // Key didn't exist or was already expired
        out_int(out, 0);
    } else {
        detach_and_free_entry(ent);
        out_int(out, 1);
    }
}

static void do_exists(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'exists' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    out_int(out, ent ? 1 : 0);
}

// --------------------------------------------------------------------------
// KEYS command — Returns all live keys as an array of strings.
// --------------------------------------------------------------------------
static void do_keys(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    (void)cmd;  // KEYS takes no arguments
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

    out_arr(out, (uint32_t)keys.size());
    for (const std::string &k : keys) {
        out_str(out, k);
    }
}

// --------------------------------------------------------------------------
// ZSet commands
// --------------------------------------------------------------------------

static void do_zadd(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 4) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zadd' command");
        return;
    }
    double score = 0;
    try {
        score = std::stod(cmd[2]);
    } catch (...) {
        out_err(out, ERR_BADARGS, "ERR value is not a valid float");
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
            out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
            return;
        }
    }
    
    bool added = zset_add(ent->zset, cmd[3].data(), cmd[3].size(), score);
    out_int(out, added ? 1 : 0);
}

static void do_zscore(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zscore' command");
        return;
    }
    
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_nil(out);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    ZNode *zn = zset_lookup(ent->zset, cmd[2].data(), cmd[2].size());
    if (zn) {
        out_dbl(out, zn->score);
    } else {
        out_nil(out);
    }
}

static void do_zrem(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zrem' command");
        return;
    }
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_int(out, 0);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    ZNode *zn = zset_pop(ent->zset, cmd[2].data(), cmd[2].size());
    if (zn) {
        delete zn;
        out_int(out, 1);
    } else {
        out_int(out, 0);
    }
}

static void do_zrank(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zrank' command");
        return;
    }
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_nil(out);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    int64_t rank = zset_rank(ent->zset, cmd[2].data(), cmd[2].size());
    if (rank >= 0) {
        out_int(out, rank);
    } else {
        out_nil(out);
    }
}

static void do_zquery(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 6) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zquery' command");
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
        out_err(out, ERR_BADARGS, "ERR value is not a valid float or integer");
        return;
    }
    
    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_arr(out, 0);
        return;
    }
    if (ent->type != T_ZSET) {
        out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }
    
    std::vector<std::pair<std::string, double>> res;
    zset_query(ent->zset, score, cmd[3].data(), cmd[3].size(), offset, limit, res);
    
    out_arr(out, (uint32_t)(res.size() * 2));
    for (const auto &p : res) {
        out_str(out, p.first);
        out_dbl(out, p.second);
    }
}

// --------------------------------------------------------------------------
// TTL commands (Chapter 13)
// --------------------------------------------------------------------------

static void do_expire(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'expire' command");
        return;
    }

    int64_t ttl_sec = 0;
    try {
        ttl_sec = std::stoll(cmd[2]);
    } catch (...) {
        out_err(out, ERR_BADARGS, "ERR value is not an integer or out of range");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        // Key doesn't exist or already expired
        out_int(out, 0);
        return;
    }

    if (ttl_sec <= 0) {
        // Non-positive TTL expires and deletes the key immediately
        detach_and_free_entry(ent);
        out_int(out, 1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = now_us + (uint64_t)ttl_sec * 1000000ULL;

    if (ent->heap_idx != (size_t)-1) {
        // Update existing heap position in O(log n)
        heap_update_val(g_heap, ent->heap_idx, expire_us);
    } else {
        // Push new heap node in O(log n)
        heap_insert(g_heap, expire_us, &ent->heap_idx);
    }

    out_int(out, 1);
}

static void do_pexpire(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'pexpire' command");
        return;
    }

    int64_t ttl_ms = 0;
    try {
        ttl_ms = std::stoll(cmd[2]);
    } catch (...) {
        out_err(out, ERR_BADARGS, "ERR value is not an integer or out of range");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_int(out, 0);
        return;
    }

    if (ttl_ms <= 0) {
        detach_and_free_entry(ent);
        out_int(out, 1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = now_us + (uint64_t)ttl_ms * 1000ULL;

    if (ent->heap_idx != (size_t)-1) {
        heap_update_val(g_heap, ent->heap_idx, expire_us);
    } else {
        heap_insert(g_heap, expire_us, &ent->heap_idx);
    }

    out_int(out, 1);
}

static void do_ttl(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'ttl' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        // -2 if the key does not exist (or has expired)
        out_int(out, -2);
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        // -1 if the key exists but has no associated expire
        out_int(out, -1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = g_heap[ent->heap_idx].val;
    if (expire_us <= now_us) {
        detach_and_free_entry(ent);
        out_int(out, -2);
        return;
    }

    // Ceil division so that remaining time > 0 returns at least 1 second
    int64_t remain_sec = (int64_t)((expire_us - now_us + 999999ULL) / 1000000ULL);
    out_int(out, remain_sec);
}

static void do_pttl(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'pttl' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_int(out, -2);
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        out_int(out, -1);
        return;
    }

    uint64_t now_us = get_monotonic_usec();
    uint64_t expire_us = g_heap[ent->heap_idx].val;
    if (expire_us <= now_us) {
        detach_and_free_entry(ent);
        out_int(out, -2);
        return;
    }

    int64_t remain_ms = (int64_t)((expire_us - now_us + 999ULL) / 1000ULL);
    out_int(out, remain_ms);
}

static void do_persist(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'persist' command");
        return;
    }

    Entry *ent = get_live_entry(cmd[1]);
    if (!ent) {
        out_int(out, 0); // Key does not exist
        return;
    }

    if (ent->heap_idx == (size_t)-1) {
        out_int(out, 0); // Key has no associated timeout
        return;
    }

    // Remove from heap and reset heap_idx to (size_t)-1
    heap_delete(g_heap, ent->heap_idx);
    out_int(out, 1); // Timeout removed
}

// --------------------------------------------------------------------------
// Utility and Benchmark helper commands (PING, CONFIG, ZPOPULATE)
// --------------------------------------------------------------------------

static void do_ping(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() == 1) {
        out_str(out, "PONG");
    } else if (cmd.size() == 2) {
        out_str(out, cmd[1]);
    } else {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'ping' command");
    }
}

static void do_config(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() >= 3 && (cmd[1] == "set" || cmd[1] == "SET")) {
        if (cmd[2] == "lazyfree" || cmd[2] == "LAZYFREE") {
            if (cmd.size() < 4) {
                out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'config set'");
                return;
            }
            if (cmd[3] == "1" || cmd[3] == "yes" || cmd[3] == "true") {
                g_lazyfree_enabled = true;
                out_str(out, "OK");
            } else if (cmd[3] == "0" || cmd[3] == "no" || cmd[3] == "false") {
                g_lazyfree_enabled = false;
                out_str(out, "OK");
            } else {
                out_err(out, ERR_BADARGS, "ERR value must be 0 or 1");
            }
            return;
        }
    } else if (cmd.size() >= 3 && (cmd[1] == "get" || cmd[1] == "GET")) {
        if (cmd[2] == "lazyfree" || cmd[2] == "LAZYFREE") {
            out_arr(out, 2);
            out_str(out, "lazyfree");
            out_str(out, g_lazyfree_enabled ? "yes" : "no");
            return;
        }
    }
    out_err(out, ERR_UNKNOWN, "ERR unsupported config command");
}

static void do_zpopulate(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'zpopulate' command");
        return;
    }
    int64_t count = 0;
    try {
        count = std::stoll(cmd[2]);
    } catch (...) {
        out_err(out, ERR_BADARGS, "ERR invalid count");
        return;
    }
    if (count <= 0) {
        out_int(out, 0);
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
        out_err(out, ERR_BADARGS, "WRONGTYPE Operation against a key holding the wrong kind of value");
        return;
    }

    int64_t added = 0;
    for (int64_t i = 0; i < count; i++) {
        std::string name = "m" + std::to_string(i);
        if (zset_add(ent->zset, name.data(), name.size(), (double)i)) {
            added++;
        }
    }
    out_int(out, added);
}

// --------------------------------------------------------------------------
// Command dispatch — the central router.
// --------------------------------------------------------------------------
static void do_request(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() == 0) {
        out_err(out, ERR_UNKNOWN, "ERR empty command");
        return;
    }

    // Case-insensitive command matching.
    if (cmd[0] == "get" || cmd[0] == "GET") {
        do_get(cmd, out);
    } else if (cmd[0] == "set" || cmd[0] == "SET") {
        do_set(cmd, out);
    } else if (cmd[0] == "del" || cmd[0] == "DEL" || cmd[0] == "unlink" || cmd[0] == "UNLINK") {
        do_del(cmd, out);
    } else if (cmd[0] == "ping" || cmd[0] == "PING") {
        do_ping(cmd, out);
    } else if (cmd[0] == "config" || cmd[0] == "CONFIG") {
        do_config(cmd, out);
    } else if (cmd[0] == "zpopulate" || cmd[0] == "ZPOPULATE") {
        do_zpopulate(cmd, out);
    } else if (cmd[0] == "exists" || cmd[0] == "EXISTS") {
        do_exists(cmd, out);
    } else if (cmd[0] == "keys" || cmd[0] == "KEYS") {
        do_keys(cmd, out);
    } else if (cmd[0] == "expire" || cmd[0] == "EXPIRE") {
        do_expire(cmd, out);
    } else if (cmd[0] == "pexpire" || cmd[0] == "PEXPIRE") {
        do_pexpire(cmd, out);
    } else if (cmd[0] == "ttl" || cmd[0] == "TTL") {
        do_ttl(cmd, out);
    } else if (cmd[0] == "pttl" || cmd[0] == "PTTL") {
        do_pttl(cmd, out);
    } else if (cmd[0] == "persist" || cmd[0] == "PERSIST") {
        do_persist(cmd, out);
    } else if (cmd[0] == "zadd" || cmd[0] == "ZADD") {
        do_zadd(cmd, out);
    } else if (cmd[0] == "zscore" || cmd[0] == "ZSCORE") {
        do_zscore(cmd, out);
    } else if (cmd[0] == "zrem" || cmd[0] == "ZREM") {
        do_zrem(cmd, out);
    } else if (cmd[0] == "zrank" || cmd[0] == "ZRANK") {
        do_zrank(cmd, out);
    } else if (cmd[0] == "zquery" || cmd[0] == "ZQUERY") {
        do_zquery(cmd, out);
    } else {
        out_err(out, ERR_UNKNOWN, "ERR unknown command");
    }
}

// --------------------------------------------------------------------------
// Response serialization — write the typed output into the connection's wbuf.
//
// BEFORE: serialize_res(status, data, conn) — wrote [total_len][status][data]
// AFTER:  serialize_res(out, conn) — writes [total_len][serialized typed value]
//
// The "status" is gone — it's embedded in the type tag. The outer frame is
// still [4-byte LE length][payload], same as before. Only the payload format
// changed from [status:u32][raw bytes] to [type_tag:u8][type-specific data].
// --------------------------------------------------------------------------
static void serialize_res(const std::vector<uint8_t> &out, Conn *conn) {
    uint32_t total_len = (uint32_t)out.size();
    memcpy(conn->wbuf, &total_len, 4);
    if (out.size() > 0) {
        memcpy(conn->wbuf + 4, out.data(), out.size());
    }
    conn->wbuf_size = 4 + total_len;
    conn->wbuf_sent = 0;
    conn->state = STATE_WRITE;
}

// --------------------------------------------------------------------------
// try_one_request — parse one request from rbuf, execute it, serialize the
// response into wbuf.
// --------------------------------------------------------------------------
static bool try_one_request(Conn *conn) {
    if (conn->rbuf_size < 4) {
        return false;
    }

    uint32_t len = 0;
    memcpy(&len, conn->rbuf, 4);

    if (len > k_max_msg) {
        fprintf(stderr, "try_one_request: message too long: %u bytes (max %u)\n",
                len, (unsigned)k_max_msg);
        conn->want_close = true;
        return false;
    }

    if (conn->rbuf_size < 4 + len) {
        return false;   // not yet — wait for more bytes from the next recv()
    }

    // Phase 1: Parse
    std::vector<std::string> cmd;
    if (!parse_req(&conn->rbuf[4], len, cmd)) {
        fprintf(stderr, "try_one_request: failed to parse request format\n");
        conn->want_close = true;
        return false;
    }

    // Phase 2: Execute + Serialize into typed output
    std::vector<uint8_t> out;
    do_request(cmd, out);

    // Phase 3: Frame and write to wbuf
    serialize_res(out, conn);

    // Compact rbuf: remove the consumed message
    size_t consumed = 4 + len;
    size_t remaining = conn->rbuf_size - consumed;
    if (remaining > 0) {
        memmove(conn->rbuf, &conn->rbuf[consumed], remaining);
    }
    conn->rbuf_size = remaining;

    return true;
}

// --------------------------------------------------------------------------
// state_read — handle a POLLIN event for this connection.
// --------------------------------------------------------------------------
static void state_read(Conn *conn) {
    size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    if (cap == 0) {
        if (!try_one_request(conn)) {
            fprintf(stderr, "state_read: rbuf full, cannot parse\n");
            conn->want_close = true;
        }
        return;
    }

    int rv = recv(conn->fd,
                  (char *)&conn->rbuf[conn->rbuf_size],
                  (int)cap,
                  0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return;
        }
        fprintf(stderr, "recv() error on fd %u: %d\n",
                (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        printf("Client disconnected (fd=%u).\n",
               (unsigned)conn->fd);
        conn->want_close = true;
        return;
    }

    conn->rbuf_size += (size_t)rv;

    // Moving connection to the tail of the idle list whenever it does IO
    conn_touch(conn);

    while (try_one_request(conn)) {
        if (conn->state == STATE_WRITE) {
            return;
        }
    }
}

// --------------------------------------------------------------------------
// state_write — handle a POLLOUT event for this connection.
// --------------------------------------------------------------------------
static void state_write(Conn *conn) {
    int rv = send(conn->fd,
                  (const char *)&conn->wbuf[conn->wbuf_sent],
                  (int)(conn->wbuf_size - conn->wbuf_sent),
                  0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            return;
        }
        fprintf(stderr, "send() error on fd %u: %d\n",
                (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    conn->wbuf_sent += (size_t)rv;

    // Moving connection to the tail of the idle list whenever it does IO
    conn_touch(conn);

    if (conn->wbuf_sent == conn->wbuf_size) {
        conn->state = STATE_READ;
        conn->wbuf_size = 0;
        conn->wbuf_sent = 0;

        while (try_one_request(conn)) {
            if (conn->state == STATE_WRITE) {
                return;
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
        memset(conn, 0, sizeof(Conn));
        conn->fd = conn_fd;
        conn->state = STATE_READ;
        conn->want_close = false;
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
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
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
    addr.sin_port = htons(1234);
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

    printf("Server listening on 127.0.0.1:1234 (event loop + thread pool mode) ...\n");

    dlist_init(&g_idle_list);
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
