// test_ttl.cpp — Integration tests for Chapter 13: TTL with a heap.
//
// Tests:
// 1. Basic EXPIRE / TTL / PERSIST commands and Redis semantics
// 2. Active reclaim: a key that expires while idle (no client traffic)
// 3. Lazy path: a key checked at/after its expiry boundary
// 4. TTL renewal: updating heap position correctly (sift-down and sift-up)

#define WIN32_LEAN_AND_MEAN
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include "../src/serialization.h"

static const size_t k_max_msg = 4096;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: %s\n", __LINE__, msg); \
            exit(1); \
        } \
    } while (0)

static int write_full(SOCKET fd, const char *buf, size_t n) {
    while (n > 0) {
        int rv = send(fd, buf, (int)n, 0);
        if (rv <= 0) return -1;
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

static int read_full(SOCKET fd, char *buf, size_t n) {
    while (n > 0) {
        int rv = recv(fd, buf, (int)n, 0);
        if (rv <= 0) return -1;
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

static int send_request(SOCKET fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + (uint32_t)s.size();
    }
    char header[4];
    memcpy(header, &len, 4);
    if (write_full(fd, header, 4) != 0) return -1;

    uint32_t nstr = (uint32_t)cmd.size();
    char nstr_buf[4];
    memcpy(nstr_buf, &nstr, 4);
    if (write_full(fd, nstr_buf, 4) != 0) return -1;

    for (const std::string &s : cmd) {
        uint32_t slen = (uint32_t)s.size();
        char slen_buf[4];
        memcpy(slen_buf, &slen, 4);
        if (write_full(fd, slen_buf, 4) != 0) return -1;
        if (write_full(fd, s.data(), s.size()) != 0) return -1;
    }
    return 0;
}

static int read_response_raw(SOCKET fd, std::vector<uint8_t> &out) {
    char header[4];
    if (read_full(fd, header, 4) != 0) return -1;
    uint32_t len = 0;
    memcpy(&len, header, 4);
    if (len > k_max_msg) return -1;
    out.resize(len);
    if (len > 0) {
        if (read_full(fd, (char *)out.data(), len) != 0) return -1;
    }
    return 0;
}

static SOCKET connect_server() {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

static int64_t req_int(SOCKET fd, const std::vector<std::string> &cmd) {
    CHECK(send_request(fd, cmd) == 0, "send_request failed");
    std::vector<uint8_t> resp;
    CHECK(read_response_raw(fd, resp) == 0, "read_response failed");
    CHECK(resp.size() >= 1 && resp[0] == SER_INT, "Expected SER_INT response");
    const uint8_t *p = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    read_u8(p, len, tag);
    int64_t val = 0;
    CHECK(read_i64(p, len, val), "Failed to read i64");
    return val;
}

static std::string req_str(SOCKET fd, const std::vector<std::string> &cmd) {
    CHECK(send_request(fd, cmd) == 0, "send_request failed");
    std::vector<uint8_t> resp;
    CHECK(read_response_raw(fd, resp) == 0, "read_response failed");
    CHECK(resp.size() >= 1 && resp[0] == SER_STR, "Expected SER_STR response");
    const uint8_t *p = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    read_u8(p, len, tag);
    std::string val;
    CHECK(read_str(p, len, val), "Failed to read str");
    return val;
}

static bool req_is_nil(SOCKET fd, const std::vector<std::string> &cmd) {
    CHECK(send_request(fd, cmd) == 0, "send_request failed");
    std::vector<uint8_t> resp;
    CHECK(read_response_raw(fd, resp) == 0, "read_response failed");
    return (resp.size() >= 1 && resp[0] == SER_NIL);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET fd = connect_server();
    CHECK(fd != INVALID_SOCKET, "Cannot connect to server (is server.exe running on 127.0.0.1:1234?)");

    printf("=== Starting TTL & Heap Integration Tests ===\n");

    // ----------------------------------------------------------------------
    // 1. Basic semantics: EXPIRE, TTL, PERSIST
    // ----------------------------------------------------------------------
    printf("\n[Test 1] Testing basic EXPIRE, TTL, PERSIST semantics...\n");
    // TTL on non-existent key returns -2
    CHECK(req_int(fd, {"ttl", "nonexistent_key_xyz"}) == -2, "TTL on nonexistent key must return -2");
    // EXPIRE on non-existent key returns 0
    CHECK(req_int(fd, {"expire", "nonexistent_key_xyz", "10"}) == 0, "EXPIRE on nonexistent key must return 0");

    // Create a key without TTL
    req_str(fd, {"set", "mykey", "val1"});
    // Persistent key returns -1 for TTL
    CHECK(req_int(fd, {"ttl", "mykey"}) == -1, "Persistent key must return -1 for TTL");
    // PERSIST on persistent key returns 0
    CHECK(req_int(fd, {"persist", "mykey"}) == 0, "PERSIST on persistent key must return 0");

    // Set TTL to 10 seconds
    CHECK(req_int(fd, {"expire", "mykey", "10"}) == 1, "EXPIRE must return 1 on success");
    int64_t ttl_val = req_int(fd, {"ttl", "mykey"});
    CHECK(ttl_val >= 9 && ttl_val <= 10, "TTL must be 9 or 10 seconds");

    // Remove TTL via PERSIST
    CHECK(req_int(fd, {"persist", "mykey"}) == 1, "PERSIST must return 1 when TTL is removed");
    CHECK(req_int(fd, {"ttl", "mykey"}) == -1, "TTL must be -1 after PERSIST");

    // Overwriting key with SET removes TTL
    req_int(fd, {"expire", "mykey", "20"});
    CHECK(req_int(fd, {"ttl", "mykey"}) > 0, "TTL should be positive");
    req_str(fd, {"set", "mykey", "val2"});
    CHECK(req_int(fd, {"ttl", "mykey"}) == -1, "SET must clear TTL");

    // Clean up
    req_int(fd, {"del", "mykey"});
    printf("[PASS] Basic EXPIRE / TTL / PERSIST semantics verified.\n");

    // ----------------------------------------------------------------------
    // 2. Active reclaim: key expires while idle (no client traffic)
    // ----------------------------------------------------------------------
    printf("\n[Test 2] Testing active reclaim while idle (event loop wakes via next_timer_ms)...\n");
    req_str(fd, {"set", "active_key", "active_val"});
    CHECK(req_int(fd, {"expire", "active_key", "1"}) == 1, "Set 1s TTL on active_key");

    printf("Sleeping for 1.3 seconds with NO client traffic...\n");
    Sleep(1300);

    // Server must have actively reclaimed active_key during idle wait
    CHECK(req_int(fd, {"exists", "active_key"}) == 0, "Key must not exist after active reclaim");
    CHECK(req_is_nil(fd, {"get", "active_key"}), "GET on actively reclaimed key must return nil");
    CHECK(req_int(fd, {"ttl", "active_key"}) == -2, "TTL must return -2 for expired key");
    printf("[PASS] Active reclaim while idle succeeded.\n");

    // ----------------------------------------------------------------------
    // 3. Lazy path: key checked at/after expiry boundary
    // ----------------------------------------------------------------------
    printf("\n[Test 3] Testing lazy expiration on access (boundary check)...\n");
    req_str(fd, {"set", "lazy_key", "lazy_val"});
    // Use PEXPIRE for sub-second precision: 350ms
    CHECK(req_int(fd, {"pexpire", "lazy_key", "350"}) == 1, "Set 350ms TTL on lazy_key");

    // Sleep exactly 400ms to cross the expiration boundary
    Sleep(400);

    // Now query: lazy path in get_live_entry must catch and delete it
    CHECK(req_is_nil(fd, {"get", "lazy_key"}), "GET must return nil on lazy-expired key");
    CHECK(req_int(fd, {"exists", "lazy_key"}) == 0, "EXISTS must return 0");
    printf("[PASS] Lazy expiration boundary check succeeded.\n");

    // ----------------------------------------------------------------------
    // 4. TTL renewal: second EXPIRE updates heap position correctly
    // ----------------------------------------------------------------------
    printf("\n[Test 4] Testing TTL renewal and heap repositioning...\n");

    // Test 4A: Extending TTL (sift-down)
    // Key A has 1s TTL, Key B has 3s TTL.
    // Initially: Key A is min (at root), expires first.
    req_str(fd, {"set", "key_a", "val_a"});
    req_str(fd, {"set", "key_b", "val_b"});
    req_int(fd, {"expire", "key_a", "1"});
    req_int(fd, {"expire", "key_b", "3"});

    // Now renew Key A to 5 seconds. Key A now expires AFTER Key B!
    // Heap must sift Key A down so Key B becomes the root.
    CHECK(req_int(fd, {"expire", "key_a", "5"}) == 1, "Renew key_a to 5s");

    // Wait 1.5 seconds. Key A would have died under original 1s TTL.
    printf("Waiting 1.5s: key_a was renewed to 5s, so it must still be alive...\n");
    Sleep(1500);

    CHECK(!req_is_nil(fd, {"get", "key_a"}), "key_a must still be alive!");
    CHECK(req_str(fd, {"get", "key_a"}) == "val_a", "key_a value intact");
    int64_t ttl_a = req_int(fd, {"ttl", "key_a"});
    CHECK(ttl_a >= 2 && ttl_a <= 4, "key_a should have ~3-4s TTL left");

    // Wait another 1.8 seconds (total 3.3s elapsed):
    // Key B (TTL 3s) must now be dead. Key A (TTL 5s) must still be alive!
    printf("Waiting another 1.8s: key_b (TTL 3s) should expire, key_a still alive...\n");
    Sleep(1800);

    CHECK(req_is_nil(fd, {"get", "key_b"}), "key_b should have expired at 3s");
    CHECK(!req_is_nil(fd, {"get", "key_a"}), "key_a must still be alive at ~3.3s");

    // Wait another 2.0s (total > 5s elapsed):
    // Key A must now expire.
    Sleep(2000);
    CHECK(req_is_nil(fd, {"get", "key_a"}), "key_a must now expire at 5s");

    // Test 4B: Shortening TTL (sift-up)
    // Key C has 10s TTL. Key D has 4s TTL.
    req_str(fd, {"set", "key_c", "val_c"});
    req_str(fd, {"set", "key_d", "val_d"});
    req_int(fd, {"expire", "key_c", "10"});
    req_int(fd, {"expire", "key_d", "4"});

    // Shorten Key C to 1s. Key C must sift UP to become the new root.
    CHECK(req_int(fd, {"expire", "key_c", "1"}) == 1, "Shorten key_c to 1s");

    // Wait 1.4 seconds: Key C must be expired, Key D must still be alive.
    Sleep(1400);
    CHECK(req_is_nil(fd, {"get", "key_c"}), "key_c must have expired early");
    CHECK(!req_is_nil(fd, {"get", "key_d"}), "key_d must still be alive");

    // Clean up
    req_int(fd, {"del", "key_d"});
    printf("[PASS] TTL renewal and heap repositioning (both sift-up and sift-down) verified!\n");

    printf("\n===========================================\n");
    printf("[SUCCESS] All Chapter 13 TTL tests passed!\n");
    printf("===========================================\n");

    closesocket(fd);
    WSACleanup();
    return 0;
}
