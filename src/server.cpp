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

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include "hashtable.h"
#include "serialization.h"

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
struct Entry {
    HNode  node;           // intrusive hash node (must be a member, not a pointer)
    std::string key;
    std::string val;
};

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

// The global key-value store.
static HMap g_map = {};

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
};

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

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    if (!node) {
        // Key not found — return NIL, not an error.
        // This is semantically correct: "no value" is different from "something went wrong".
        out_nil(out);
    } else {
        Entry *ent = container_of(node, Entry, node);
        out_str(out, ent->val);
    }
}

static void do_set(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 3) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'set' command");
        return;
    }

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    if (node) {
        // Key exists — update value in place
        Entry *ent = container_of(node, Entry, node);
        ent->val = cmd[2];
    } else {
        // Key doesn't exist — allocate a new Entry and insert
        Entry *ent = new Entry();
        ent->key = cmd[1];
        ent->val = cmd[2];
        ent->node.hcode = key.node.hcode;
        hm_insert(&g_map, &ent->node);
    }

    // SET returns a simple string "OK", matching real Redis behavior.
    out_str(out, "OK");
}

static void do_del(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'del' command");
        return;
    }

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_delete(&g_map, &key.node, &entry_eq);
    if (!node) {
        // Key didn't exist — deleted 0 keys.
        // NOW returns an integer 0 instead of the string "0".
        out_int(out, 0);
    } else {
        Entry *ent = container_of(node, Entry, node);
        delete ent;
        // Deleted 1 key — return integer 1.
        out_int(out, 1);
    }
}

static void do_exists(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    if (cmd.size() != 2) {
        out_err(out, ERR_BADARGS, "ERR wrong number of arguments for 'exists' command");
        return;
    }

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    // EXISTS returns integer 1 (found) or 0 (not found).
    // In real Redis, EXISTS can take multiple keys and returns the count of
    // keys that exist. We only support single-key for now.
    out_int(out, node ? 1 : 0);
}

// --------------------------------------------------------------------------
// KEYS command — new! Returns all keys as an array of strings.
//
// This is a good demo of the SER_ARR type: the response is an array where
// each element is a SER_STR. In real Redis, KEYS takes a glob pattern;
// we return all keys unconditionally for simplicity.
//
// WARNING: Like real Redis's KEYS command, this is O(N) in the number of
// keys and should never be used in production on a large database. We
// include it here because it's the simplest way to exercise array responses.
// --------------------------------------------------------------------------
static void cb_scan(HNode *node, void *arg) {
    std::vector<std::string> *keys = (std::vector<std::string> *)arg;
    Entry *ent = container_of(node, Entry, node);
    keys->push_back(ent->key);
}

static void do_keys(const std::vector<std::string> &cmd, std::vector<uint8_t> &out) {
    (void)cmd;  // KEYS takes no arguments (simplified — no glob pattern)

    // Collect all keys by scanning both hash tables.
    std::vector<std::string> keys;
    // Scan ht1
    if (g_map.ht1.tab) {
        for (size_t i = 0; i <= g_map.ht1.mask; i++) {
            HNode *node = g_map.ht1.tab[i];
            while (node) {
                cb_scan(node, &keys);
                node = node->next;
            }
        }
    }
    // Scan ht2 (active during rehashing)
    if (g_map.ht2.tab) {
        for (size_t i = 0; i <= g_map.ht2.mask; i++) {
            HNode *node = g_map.ht2.tab[i];
            while (node) {
                cb_scan(node, &keys);
                node = node->next;
            }
        }
    }

    // Serialize as an array of strings
    out_arr(out, (uint32_t)keys.size());
    for (const std::string &k : keys) {
        out_str(out, k);
    }
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
    // We compare against both cases manually — simple and avoids pulling in
    // <algorithm> for std::transform. A real server would normalize once.
    if (cmd[0] == "get" || cmd[0] == "GET") {
        do_get(cmd, out);
    } else if (cmd[0] == "set" || cmd[0] == "SET") {
        do_set(cmd, out);
    } else if (cmd[0] == "del" || cmd[0] == "DEL") {
        do_del(cmd, out);
    } else if (cmd[0] == "exists" || cmd[0] == "EXISTS") {
        do_exists(cmd, out);
    } else if (cmd[0] == "keys" || cmd[0] == "KEYS") {
        do_keys(cmd, out);
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

    printf("Server listening on 127.0.0.1:1234 (event loop mode) ...\n");

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

        // ---- Step 2: WSAPoll — wait for events ----
        int rv = WSAPoll(pfds.data(), (ULONG)pfds.size(), 1000);
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

        // ---- Step 5: Sweep closed connections ----
        for (int i = (int)connections.size() - 1; i >= 0; i--) {
            if (connections[i]->want_close) {
                Conn *conn = connections[i];
                printf("Closing connection fd=%u.\n",
                       (unsigned)conn->fd);
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
