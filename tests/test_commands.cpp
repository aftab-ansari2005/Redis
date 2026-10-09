// test_commands.cpp — Chapter 9: Verify typed serialization for GET, SET, DEL, EXISTS, KEYS.
//
// This test was rewritten for the new typed response format. Instead of
// checking (status, string) pairs, we now check (type_tag, typed_value) tuples.
//
// The test infrastructure deserializes responses and verifies both the type
// and the value, which catches mismatches like "DEL returning a string '1'
// instead of integer 1" that the old tests couldn't distinguish.

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
#include "../src/serialization.h"

static const size_t k_max_msg = 4096;

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
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

static int write_full(SOCKET fd, const char *buf, size_t n) {
    while (n > 0) {
        int rv = send(fd, buf, (int)n, 0);
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
    if (write_full(fd, (char*)&nstr, 4) != 0) return -1;

    for (const std::string &s : cmd) {
        uint32_t slen = (uint32_t)s.size();
        if (write_full(fd, (char*)&slen, 4) != 0) return -1;
        if (write_full(fd, s.data(), slen) != 0) return -1;
    }
    return 0;
}

// Read the raw response payload (after the 4-byte outer length header).
static int read_response_raw(SOCKET fd, std::vector<uint8_t> &out) {
    char header[4];
    if (read_full(fd, header, 4) != 0) return -1;

    uint32_t len = 0;
    memcpy(&len, header, 4);
    if (len > k_max_msg) return -1;

    out.resize(len);
    if (len > 0) {
        if (read_full(fd, (char*)out.data(), len) != 0) return -1;
    }
    return 0;
}

// --------------------------------------------------------------------------
// Test helpers for typed responses
// --------------------------------------------------------------------------

static void print_cmd(const std::vector<std::string> &cmd) {
    for (const auto &s : cmd) printf("%s ", s.c_str());
}

// Expect a NIL response.
static int test_nil(SOCKET fd, const std::vector<std::string> &cmd) {
    if (send_request(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    std::vector<uint8_t> resp;
    if (read_response_raw(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.size() < 1 || resp[0] != SER_NIL) {
        printf("[FAIL] Expected NIL, got tag %u — ", resp.empty() ? 255 : resp[0]);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (nil)  ");
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect a string response with a specific value.
static int test_str(SOCKET fd, const std::vector<std::string> &cmd, const std::string &expected) {
    if (send_request(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    std::vector<uint8_t> resp;
    if (read_response_raw(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    const uint8_t *data = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    if (!read_u8(data, len, tag) || tag != SER_STR) {
        printf("[FAIL] Expected STR, got tag %u — ", tag);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    std::string val;
    if (!read_str(data, len, val)) {
        printf("[FAIL] Malformed STR payload — ");
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    if (val != expected) {
        printf("[FAIL] Expected str '%s', got '%s' — ", expected.c_str(), val.c_str());
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (str) \"%s\"  ", val.c_str());
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an integer response with a specific value.
static int test_int(SOCKET fd, const std::vector<std::string> &cmd, int64_t expected) {
    if (send_request(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    std::vector<uint8_t> resp;
    if (read_response_raw(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    const uint8_t *data = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    if (!read_u8(data, len, tag) || tag != SER_INT) {
        printf("[FAIL] Expected INT, got tag %u — ", tag);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    int64_t val;
    if (!read_i64(data, len, val)) {
        printf("[FAIL] Malformed INT payload — ");
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    if (val != expected) {
        printf("[FAIL] Expected int %" PRId64 ", got %" PRId64 " — ", expected, val);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (int) %" PRId64 "  ", val);
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an error response (any error code, check message substring).
static int test_err(SOCKET fd, const std::vector<std::string> &cmd, const std::string &msg_substr) {
    if (send_request(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    std::vector<uint8_t> resp;
    if (read_response_raw(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    const uint8_t *data = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    if (!read_u8(data, len, tag) || tag != SER_ERR) {
        printf("[FAIL] Expected ERR, got tag %u — ", tag);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    uint32_t code;
    if (!read_u32(data, len, code)) return 1;
    std::string msg;
    if (!read_str(data, len, msg)) return 1;

    if (msg.find(msg_substr) == std::string::npos) {
        printf("[FAIL] Expected error containing '%s', got '%s' — ", msg_substr.c_str(), msg.c_str());
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (err) %u %s  ", code, msg.c_str());
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an array response of a specific length (don't check contents).
static int test_arr_len(SOCKET fd, const std::vector<std::string> &cmd, uint32_t expected_len) {
    if (send_request(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    std::vector<uint8_t> resp;
    if (read_response_raw(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    const uint8_t *data = resp.data();
    size_t len = resp.size();
    uint8_t tag;
    if (!read_u8(data, len, tag) || tag != SER_ARR) {
        printf("[FAIL] Expected ARR, got tag %u — ", tag);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    uint32_t count;
    if (!read_u32(data, len, count)) return 1;

    if (count != expected_len) {
        printf("[FAIL] Expected arr len %u, got %u — ", expected_len, count);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (arr) len=%u  ", count);
    print_cmd(cmd);
    printf("\n");
    return 0;
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("connect()");
    }

    int failures = 0;

    printf("=== Typed Serialization Tests (Chapter 9) ===\n\n");

    // Test 1: GET non-existent key → NIL
    failures += test_nil(fd, {"GET", "key1"});

    // Test 2: SET key → STR "OK"
    failures += test_str(fd, {"SET", "key1", "val1"}, "OK");

    // Test 3: GET existing key → STR "val1"
    failures += test_str(fd, {"GET", "key1"}, "val1");

    // Test 4: DEL key → INT 1  (not string "1"!)
    failures += test_int(fd, {"DEL", "key1"}, 1);

    // Test 5: GET deleted key → NIL
    failures += test_nil(fd, {"GET", "key1"});

    // Test 6: DEL non-existent key → INT 0
    failures += test_int(fd, {"DEL", "key1"}, 0);

    // Test 7: Invalid command → ERR
    failures += test_err(fd, {"FOO"}, "unknown command");

    // Test 8: SET wrong arguments → ERR
    failures += test_err(fd, {"SET", "k"}, "wrong number of arguments");

    // Test 9: EXISTS on non-existent key → INT 0
    failures += test_int(fd, {"EXISTS", "nokey"}, 0);

    // Test 10: SET then EXISTS → INT 1
    failures += test_str(fd, {"SET", "ekey", "eval"}, "OK");
    failures += test_int(fd, {"EXISTS", "ekey"}, 1);

    // Test 11: DEL then EXISTS → INT 0
    failures += test_int(fd, {"DEL", "ekey"}, 1);
    failures += test_int(fd, {"EXISTS", "ekey"}, 0);

    // Test 12: EXISTS wrong number of arguments → ERR
    failures += test_err(fd, {"EXISTS"}, "wrong number of arguments");

    // Test 13: KEYS on empty store → ARR len=0
    failures += test_arr_len(fd, {"KEYS"}, 0);

    // Test 14: SET two keys, then KEYS → ARR len=2
    failures += test_str(fd, {"SET", "k1", "v1"}, "OK");
    failures += test_str(fd, {"SET", "k2", "v2"}, "OK");
    failures += test_arr_len(fd, {"KEYS"}, 2);

    // Clean up test keys
    failures += test_int(fd, {"DEL", "k1"}, 1);
    failures += test_int(fd, {"DEL", "k2"}, 1);

    printf("\n");
    if (failures == 0) {
        printf("All typed serialization tests passed!\n");
    } else {
        printf("%d tests failed.\n", failures);
    }

    closesocket(fd);
    WSACleanup();
    return failures;
}
