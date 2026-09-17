// server.cpp — Part 3: Single-threaded event loop with WSAPoll (Winsock2 / MinGW)
//
// ARCHITECTURE CHANGE from Part 2:
//   Part 2: blocking, one client at a time (accept → serve → close → accept)
//   Part 3: non-blocking, many clients concurrently via a single-threaded event loop
//
// All sockets are NON-BLOCKING. The main loop uses WSAPoll() (Windows equivalent
// of POSIX poll()) to ask the OS "which sockets are ready for IO?" and then
// processes only those sockets. No socket ever blocks the server.
//
// This is the same architecture real Redis uses (with epoll on Linux).
//
// Wire format (unchanged from Part 2):
//   [4 bytes: payload length, little-endian uint32] [payload bytes]

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
#define POLLNVAL    0x0004

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

// Response status codes
enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

// --------------------------------------------------------------------------
// Global key-value store — now backed by our custom HMap hashtable.
//
// Entry is the application-level struct that embeds an HNode for hash table
// participation. The key and value live alongside the node — no separate
// wrapper allocation needed (this is the "intrusive" pattern).
// --------------------------------------------------------------------------
struct Entry {
    HNode  node;           // intrusive hash node (must be a member, not a pointer)
    std::string key;
    std::string val;
};

// FNV-1a hash — a simple, fast, well-distributed hash for string keys.
// Not cryptographically secure (we don't need that), but excellent for
// hash table bucket distribution.
static uint64_t str_hash(const std::string &s) {
    uint64_t h = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;           // FNV prime
    }
    return h;
}

// Equality comparator for HMap operations.
// Given two HNode pointers, recover the Entry for each via container_of
// and compare their key strings.
static bool entry_eq(HNode *lhs, HNode *rhs) {
    Entry *le = container_of(lhs, Entry, node);
    Entry *re = container_of(rhs, Entry, node);
    return le->key == re->key;
}

// The global key-value store.
static HMap g_map = {};

// --------------------------------------------------------------------------
// Connection state enum.
//
// A connection is always in exactly one of these states. The event loop
// checks this to decide whether to register POLLIN or POLLOUT for the fd.
//
//   STATE_READ  — accumulating bytes for the next request
//   STATE_WRITE — draining a response to the client
//
// Transitions:
//   STATE_READ  → STATE_WRITE   (complete request parsed, response generated)
//   STATE_WRITE → STATE_READ    (response fully sent, ready for next request)
//   either      → [closed]      (error, EOF, or protocol violation)
// --------------------------------------------------------------------------
enum {
    STATE_READ = 0,
    STATE_WRITE = 1,
};

// --------------------------------------------------------------------------
// Conn — per-connection state.
//
// Each accepted client gets one of these. It tracks:
//   - The socket fd
//   - Current state (reading or writing)
//   - An incoming byte buffer (rbuf) for accumulating partial reads
//   - An outgoing byte buffer (wbuf) for draining partial writes
//
// WHY FIXED-SIZE BUFFERS?
// At 4 + 4096 = 4100 bytes per buffer, even 10,000 connections use only ~80 MB.
// Dynamic allocation (realloc, std::vector<uint8_t>) adds heap fragmentation
// and complexity for zero benefit at this message size.
// --------------------------------------------------------------------------
struct Conn {
    SOCKET   fd;
    int      state;         // STATE_READ or STATE_WRITE
    bool     want_close;    // marked for cleanup at end of event loop iteration

    // Read buffer: bytes arrive here via recv(), accumulating until we have
    // a complete [4-byte header][payload] message.
    uint8_t  rbuf[4 + k_max_msg];
    size_t   rbuf_size;     // valid bytes currently in rbuf

    // Write buffer: holds the response frame we're sending back.
    // wbuf_sent tracks how far we've gotten (for partial writes).
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
//
// On Linux:  fcntl(fd, F_SETFL, O_NONBLOCK)
// On Windows: ioctlsocket(fd, FIONBIO, &mode)
//
// In non-blocking mode, recv/send/accept return SOCKET_ERROR with
// WSAGetLastError() == WSAEWOULDBLOCK instead of blocking. This is NOT an
// error — it means "nothing to do right now, try again later."
//
// This is ESSENTIAL for the event loop. Without it, a single slow client
// would block recv() and freeze every other client.
// --------------------------------------------------------------------------
static void set_nonblocking(SOCKET fd) {
    u_long mode = 1;   // 1 = enable non-blocking
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) {
        die("ioctlsocket(FIONBIO)");
    }
}

// Forward declarations (state_read and state_write reference each other
// through try_one_request)
static void state_read(Conn *conn);
static void state_write(Conn *conn);

// --------------------------------------------------------------------------
// try_one_request & the 3-step pipeline (Parse -> Execute -> Serialize)
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

static void do_get(const std::vector<std::string> &cmd, uint32_t &status, std::vector<uint8_t> &res) {
    if (cmd.size() != 2) {
        status = RES_ERR;
        const char *msg = "ERR wrong number of arguments for 'get' command";
        res.assign(msg, msg + strlen(msg));
        return;
    }

    // Build a temporary key-only Entry for lookup.
    // We only need .node.hcode and .key — the comparator uses container_of
    // to access .key, and the hash table uses .node.hcode for bucket indexing.
    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    if (!node) {
        status = RES_NX;
    } else {
        Entry *ent = container_of(node, Entry, node);
        status = RES_OK;
        res.assign(ent->val.begin(), ent->val.end());
    }
}

static void do_set(const std::vector<std::string> &cmd, uint32_t &status, std::vector<uint8_t> &res) {
    if (cmd.size() != 3) {
        status = RES_ERR;
        const char *msg = "ERR wrong number of arguments for 'set' command";
        res.assign(msg, msg + strlen(msg));
        return;
    }

    // SET is an "upsert": if the key exists, update the value;
    // if not, create a new Entry.
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

    status = RES_OK;
    const char *msg = "OK";
    res.assign(msg, msg + strlen(msg));
}

static void do_del(const std::vector<std::string> &cmd, uint32_t &status, std::vector<uint8_t> &res) {
    if (cmd.size() != 2) {
        status = RES_ERR;
        const char *msg = "ERR wrong number of arguments for 'del' command";
        res.assign(msg, msg + strlen(msg));
        return;
    }

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_delete(&g_map, &key.node, &entry_eq);
    if (!node) {
        status = RES_OK;
        const char *msg = "0"; // 0 deleted
        res.assign(msg, msg + strlen(msg));
    } else {
        // Got the detached node — free the Entry
        Entry *ent = container_of(node, Entry, node);
        delete ent;
        status = RES_OK;
        const char *msg = "1"; // 1 deleted
        res.assign(msg, msg + strlen(msg));
    }
}

static void do_exists(const std::vector<std::string> &cmd, uint32_t &status, std::vector<uint8_t> &res) {
    if (cmd.size() != 2) {
        status = RES_ERR;
        const char *msg = "ERR wrong number of arguments for 'exists' command";
        res.assign(msg, msg + strlen(msg));
        return;
    }

    Entry key;
    key.node.hcode = str_hash(cmd[1]);
    key.key = cmd[1];

    HNode *node = hm_lookup(&g_map, &key.node, &entry_eq);
    if (node) {
        status = RES_OK;
        const char *msg = "1";
        res.assign(msg, msg + strlen(msg));
    } else {
        status = RES_OK;
        const char *msg = "0";
        res.assign(msg, msg + strlen(msg));
    }
}

static void do_request(const std::vector<std::string> &cmd, uint32_t &status, std::vector<uint8_t> &res) {
    if (cmd.size() == 0) {
        status = RES_ERR;
        const char *msg = "ERR empty command";
        res.assign(msg, msg + strlen(msg));
        return;
    }

    if (cmd[0] == "get" || cmd[0] == "GET") {
        do_get(cmd, status, res);
    } else if (cmd[0] == "set" || cmd[0] == "SET") {
        do_set(cmd, status, res);
    } else if (cmd[0] == "del" || cmd[0] == "DEL") {
        do_del(cmd, status, res);
    } else if (cmd[0] == "exists" || cmd[0] == "EXISTS") {
        do_exists(cmd, status, res);
    } else {
        status = RES_ERR;
        const char *msg = "ERR unknown command";
        res.assign(msg, msg + strlen(msg));
    }
}

static void serialize_res(uint32_t status, const std::vector<uint8_t> &res, Conn *conn) {
    // Response format: [total_len: u32][status: u32][data...]
    uint32_t total_len = 4 + (uint32_t)res.size();
    
    // This is safe because total_len <= max_msg based on our logic, 
    // but in a real server we'd check if wbuf has enough space or resize.
    memcpy(conn->wbuf, &total_len, 4);
    memcpy(conn->wbuf + 4, &status, 4);
    if (res.size() > 0) {
        memcpy(conn->wbuf + 8, res.data(), res.size());
    }
    
    conn->wbuf_size = 4 + total_len;
    conn->wbuf_sent = 0;
    conn->state = STATE_WRITE;
}

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

    // Phase 2: Execute
    uint32_t status = RES_ERR;
    std::vector<uint8_t> res;
    do_request(cmd, status, res);

    // Phase 3: Serialize
    serialize_res(status, res, conn);

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
//
// Reads whatever bytes are available (non-blocking recv) into rbuf, then
// tries to parse a complete message.
//
// PITFALL #1: WSAEWOULDBLOCK from recv() is NOT an error. It means "no data
// available right now." Just return and wait for the next WSAPoll notification.
//
// PITFALL #2: we loop on try_one_request() because a single large recv()
// might deliver multiple pipelined messages at once.
// --------------------------------------------------------------------------
static void state_read(Conn *conn) {
    // How much room is left in rbuf?
    size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
    if (cap == 0) {
        // Buffer is completely full. This can happen when a max-size message
        // exactly fills the buffer. try_one_request should be able to parse it.
        if (!try_one_request(conn)) {
            // Can't parse and no room to read — protocol error or bug.
            fprintf(stderr, "state_read: rbuf full, cannot parse\n");
            conn->want_close = true;
        }
        return;
    }

    // Non-blocking recv: read whatever is available, up to `cap` bytes
    int rv = recv(conn->fd,
                  (char *)&conn->rbuf[conn->rbuf_size],
                  (int)cap,
                  0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            // No data right now — perfectly normal for non-blocking IO.
            return;
        }
        // Real error (WSAECONNRESET, etc.)
        fprintf(stderr, "recv() error on fd %u: %d\n",
                (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    if (rv == 0) {
        // EOF — client closed the connection gracefully.
        printf("Client disconnected (fd=%u).\n",
               (unsigned)conn->fd);
        conn->want_close = true;
        return;
    }

    // Got some bytes — add them to the buffer
    conn->rbuf_size += (size_t)rv;

    // Try to parse complete messages. We loop because recv() might have
    // delivered multiple pipelined requests in one shot.
    while (try_one_request(conn)) {
        if (conn->state == STATE_WRITE) {
            // A request was parsed and response generated — go send it.
            // Any remaining pipelined data stays in rbuf for later.
            return;
        }
    }
}

// --------------------------------------------------------------------------
// state_write — handle a POLLOUT event for this connection.
//
// Sends as many bytes from wbuf as the kernel will accept (non-blocking send).
// When the entire response is sent, switches back to STATE_READ.
//
// PITFALL #1: send() on a non-blocking socket may do a SHORT WRITE — accept
// fewer bytes than offered. We track wbuf_sent to resume on the next call.
//
// PITFALL #2: do NOT register POLLOUT when there's nothing to write.
// A socket with space in its send buffer is *always* writable, so WSAPoll
// would return immediately → busy-spin → 100% CPU for no useful work.
// We only set POLLOUT when state == STATE_WRITE (i.e., wbuf has data).
//
// PITFALL #3: after finishing a write, check rbuf for leftover pipelined
// data. If we don't, that data sits unprocessed until the client sends
// something new (which might never happen).
// --------------------------------------------------------------------------
static void state_write(Conn *conn) {
    int rv = send(conn->fd,
                  (const char *)&conn->wbuf[conn->wbuf_sent],
                  (int)(conn->wbuf_size - conn->wbuf_sent),
                  0);

    if (rv == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            // Send buffer full right now — try again next poll cycle.
            return;
        }
        fprintf(stderr, "send() error on fd %u: %d\n",
                (unsigned)conn->fd, err);
        conn->want_close = true;
        return;
    }

    // Advance the sent pointer past the bytes we just delivered
    conn->wbuf_sent += (size_t)rv;

    // Have we sent the entire response?
    if (conn->wbuf_sent == conn->wbuf_size) {
        // Response fully sent! Switch back to reading.
        conn->state = STATE_READ;
        conn->wbuf_size = 0;
        conn->wbuf_sent = 0;

        // IMPORTANT: there might be leftover bytes in rbuf from pipelined
        // requests that arrived while we were writing. Process them now
        // instead of waiting for the next poll cycle + recv.
        while (try_one_request(conn)) {
            if (conn->state == STATE_WRITE) {
                return;     // another response queued — go send it
            }
        }
    }
}

// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// conn_accept — accept all pending connections from the listener.
//
// We loop on accept() until WSAEWOULDBLOCK, draining the entire backlog.
// This is correct for level-triggered poll: even if we only accepted one,
// WSAPoll would notify us again. But draining them all is more efficient.
//
// Each new connection is set to non-blocking and added to the connections list.
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

        // New connection — make it non-blocking
        set_nonblocking(conn_fd);

        // Allocate and zero-initialize a Conn struct
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

    // IMPORTANT: make the listener non-blocking too!
    // Without this, if WSAPoll says "readable" but the pending connection was
    // RST'd between poll() and accept(), accept() would block forever.
    set_nonblocking(listen_fd);

    printf("Server listening on 127.0.0.1:1234 (event loop mode) ...\n");

    // All active client connections
    std::vector<Conn *> connections;

    // ======================================================================
    // THE EVENT LOOP
    //
    // Each iteration:
    //   1. Build the WSAPOLLFD array (listener + all connections)
    //   2. WSAPoll() — block until at least one socket is ready
    //   3. Accept new connections if the listener is ready
    //   4. Drive IO on each ready connection
    //   5. Sweep: close and free any connection marked want_close
    //
    // This single loop replaces BOTH the outer accept() loop AND the inner
    // recv/send loop from Part 2. Instead of dedicating the server to one
    // client at a time, we interleave IO across all clients in one thread.
    //
    // LEVEL-TRIGGERED semantics: WSAPoll (like POSIX poll) is level-triggered.
    // If we don't fully consume a socket's data, WSAPoll will notify us again
    // on the next call. This is simpler and more forgiving than edge-triggered
    // (epoll with EPOLLET), where you MUST drain fully or lose events.
    // ======================================================================
    while (true) {
        // ---- Step 1: Build the poll set ----
        std::vector<WSAPOLLFD> pfds;

        // Slot 0: the listener — always poll for incoming connections
        {
            WSAPOLLFD pfd = {};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            pfds.push_back(pfd);
        }

        // Slots 1..N: each client connection
        for (Conn *conn : connections) {
            WSAPOLLFD pfd = {};
            pfd.fd = conn->fd;

            if (conn->state == STATE_READ) {
                pfd.events = POLLIN;       // want to read request data
            } else {
                pfd.events = POLLOUT;      // want to write response data
            }
            // Note: POLLERR/POLLHUP are output-only — they're always reported
            // in revents regardless of what we set in events. Don't set them
            // in events: WSAPoll treats that as WSAEINVAL on Windows.
            pfds.push_back(pfd);
        }

        // ---- Step 2: WSAPoll — wait for events ----
        // Timeout = 1000ms. This means even with zero IO activity, we wake
        // up every second. We'll use this wake-up for idle-connection timeouts
        // in Part 9. For now it just prevents an infinite hang if all clients
        // disconnect while we're blocked in poll.
        int rv = WSAPoll(pfds.data(), (ULONG)pfds.size(), 1000);
        if (rv == SOCKET_ERROR) {
            die("WSAPoll()");
        }
        // rv == 0 means timeout with no ready sockets — just loop again

        // ---- Step 3: Accept new connections ----
        if (pfds[0].revents & POLLIN) {
            conn_accept(listen_fd, connections);
        }

        // ---- Step 4: Drive IO on each connection ----
        // pfds[i+1] corresponds to connections[i]
        for (size_t i = 0; i < connections.size(); i++) {
            if (pfds[i + 1].revents == 0) {
                continue;   // no events on this socket
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
        // IMPORTANT: we do this AFTER all IO processing is done, not inline
        // during the dispatch loop. Removing elements during iteration would
        // invalidate our indices (pfds[i+1] ↔ connections[i]).
        //
        // Technique: iterate backward, swap-with-last, pop_back.
        // This is O(1) per removal and doesn't shift elements.
        for (int i = (int)connections.size() - 1; i >= 0; i--) {
            if (connections[i]->want_close) {
                Conn *conn = connections[i];
                printf("Closing connection fd=%u.\n",
                       (unsigned)conn->fd);
                closesocket(conn->fd);

                // Swap with last element and pop — O(1) removal
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
