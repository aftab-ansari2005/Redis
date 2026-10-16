// test_commands.cpp — Verify RESP2 typed serialization for GET, SET, DEL, EXISTS, KEYS.

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
#include "../src/resp.h"

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

// --------------------------------------------------------------------------
// Test helpers for typed RESP2 responses
// --------------------------------------------------------------------------

static void print_cmd(const std::vector<std::string> &cmd) {
    for (const auto &s : cmd) printf("%s ", s.c_str());
}

// Expect a NIL response.
static int test_nil(SOCKET fd, const std::vector<std::string> &cmd) {
    if (resp_send_cmd(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.type != RESP_NIL) {
        printf("[FAIL] Expected NIL, got type %d — ", (int)resp.type);
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
    if (resp_send_cmd(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.type != RESP_STR) {
        printf("[FAIL] Expected STR, got type %d — ", (int)resp.type);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    if (resp.str_val != expected) {
        printf("[FAIL] Expected str '%s', got '%s' — ", expected.c_str(), resp.str_val.c_str());
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (str) \"%s\"  ", resp.str_val.c_str());
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an integer response with a specific value.
static int test_int(SOCKET fd, const std::vector<std::string> &cmd, int64_t expected) {
    if (resp_send_cmd(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.type != RESP_INT) {
        printf("[FAIL] Expected INT, got type %d — ", (int)resp.type);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    if (resp.int_val != expected) {
        printf("[FAIL] Expected int %" PRId64 ", got %" PRId64 " — ", expected, resp.int_val);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (int) %" PRId64 "  ", resp.int_val);
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an error response (check message substring).
static int test_err(SOCKET fd, const std::vector<std::string> &cmd, const std::string &msg_substr) {
    if (resp_send_cmd(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.type != RESP_ERR) {
        printf("[FAIL] Expected ERR, got type %d — ", (int)resp.type);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }

    if (resp.str_val.find(msg_substr) == std::string::npos) {
        printf("[FAIL] Expected error containing '%s', got '%s' — ", msg_substr.c_str(), resp.str_val.c_str());
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (err) %s  ", resp.str_val.c_str());
    print_cmd(cmd);
    printf("\n");
    return 0;
}

// Expect an array response of a specific length.
static int test_arr_len(SOCKET fd, const std::vector<std::string> &cmd, uint32_t expected_len) {
    if (resp_send_cmd(fd, cmd) != 0) { printf("[FAIL] send\n"); return 1; }
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) { printf("[FAIL] recv\n"); return 1; }

    if (resp.type != RESP_ARR) {
        printf("[FAIL] Expected ARR, got type %d — ", (int)resp.type);
        print_cmd(cmd);
        printf("\n");
        return 1;
    }

    if ((uint32_t)resp.arr_val.size() != expected_len) {
        printf("[FAIL] Expected arr len %u, got %u — ", expected_len, (unsigned)resp.arr_val.size());
        print_cmd(cmd);
        printf("\n");
        return 1;
    }
    printf("[PASS] (arr) len=%u  ", (unsigned)resp.arr_val.size());
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

    // Flush any leftover keys so KEYS assertions start with a clean state
    {
        resp_send_cmd(fd, {"KEYS"});
        RespValue existing;
        if (resp_read_val(fd, existing) == 0 && existing.type == RESP_ARR) {
            for (const auto &k : existing.arr_val) {
                if (k.type == RESP_STR) {
                    resp_send_cmd(fd, {"DEL", k.str_val});
                    RespValue del_resp;
                    resp_read_val(fd, del_resp);
                }
            }
        }
    }

    int failures = 0;

    printf("=== Typed Serialization Tests (RESP2) ===\n\n");

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
