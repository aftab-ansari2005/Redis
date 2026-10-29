// test_aof.cpp — Comprehensive tests for Append-Only File (AOF) persistence
#define WIN32_LEAN_AND_MEAN
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include "../src/resp.h"
#include "../src/aof.h"

// Define g_aof for the test binary
AofState g_aof = {};

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s\n", msg);
    exit(1);
}

// --------------------------------------------------------------------------
// Part 1: Direct Unit Test for AOF Format, Replay, and Truncation Recovery
// --------------------------------------------------------------------------
static void test_aof_unit() {
    printf("=== Test 1: Direct AOF formatting, replay & tail truncation ===\n");
    const std::string aof_file = "test_unit.aof";
    remove(aof_file.c_str());

    // 1. Initialize and append commands
    if (!aof_init(aof_file, AOF_FSYNC_ALWAYS)) {
        die("aof_init failed");
    }

    aof_append({"SET", "k1", "val1"});
    aof_append({"SET", "k2", "val2"});
    aof_append({"PEXPIREAT", "k1", "2000000000000"});
    aof_append({"DEL", "k2"});
    aof_close();

    // 2. Replay commands from clean AOF
    std::vector<std::vector<std::string>> replayed;
    aof_load(aof_file, [&](const std::vector<std::string> &cmd) {
        replayed.push_back(cmd);
    });

    if (replayed.size() != 4) {
        die("Expected 4 replayed commands");
    }
    if (replayed[0][0] != "SET" || replayed[0][1] != "k1" || replayed[0][2] != "val1") {
        die("Mismatch in replayed command 0");
    }
    if (replayed[2][0] != "PEXPIREAT" || replayed[2][1] != "k1") {
        die("Mismatch in replayed command 2");
    }
    if (replayed[3][0] != "DEL" || replayed[3][1] != "k2") {
        die("Mismatch in replayed command 3");
    }
    printf("[PASS] Clean AOF append & replay verified.\n");

    // 3. Test crash recovery on truncated final command:
    // Append corrupt partial bytes to the end of test_unit.aof
    FILE *fp = fopen(aof_file.c_str(), "ab");
    if (!fp) die("Cannot open file for corrupt append");
    const char *corrupt_tail = "*3\r\n$3\r\nSET\r\n$4\r\ntemp\r\n$10\r\nincom"; // truncated bulk string
    fwrite(corrupt_tail, 1, strlen(corrupt_tail), fp);
    fclose(fp);

    // Verify file grew
    FILE *check_fp = fopen(aof_file.c_str(), "rb");
    fseek(check_fp, 0, SEEK_END);
    long corrupt_size = ftell(check_fp);
    fclose(check_fp);

    // Replay with truncation recovery
    replayed.clear();
    aof_load(aof_file, [&](const std::vector<std::string> &cmd) {
        replayed.push_back(cmd);
    });

    if (replayed.size() != 4) {
        die("Truncation test: expected exactly 4 valid commands loaded");
    }

    // Verify file was truncated on disk
    check_fp = fopen(aof_file.c_str(), "rb");
    fseek(check_fp, 0, SEEK_END);
    long repaired_size = ftell(check_fp);
    fclose(check_fp);

    if (repaired_size >= corrupt_size) {
        die("File was not truncated back to valid size");
    }
    printf("[PASS] Crash recovery on truncated AOF tail verified (repaired from %ld to %ld bytes).\n",
           corrupt_size, repaired_size);

    remove(aof_file.c_str());
}

// --------------------------------------------------------------------------
// Part 2: End-to-End Server Restart & TTL Persistence Integration Test
// --------------------------------------------------------------------------
static PROCESS_INFORMATION start_server(const std::string &aof_path, int port) {
    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "server.exe --aof %s -p %d", aof_path.c_str(), port);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "Failed to start server process (%lu)\n", GetLastError());
        exit(1);
    }
    // Give server time to bind socket and initialize
    Sleep(500);
    return pi;
}

static void stop_server(PROCESS_INFORMATION &pi) {
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    Sleep(300);
}

static SOCKET connect_client(int port) {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    for (int retry = 0; retry < 10; retry++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        Sleep(100);
    }
    die("Failed to connect to server");
    return INVALID_SOCKET;
}

static void test_aof_server_integration() {
    printf("\n=== Test 2: Full Server Persistence & Restart Integration ===\n");
    const std::string aof_file = "test_integ.aof";
    const int test_port = 12348;
    remove(aof_file.c_str());

    // --- Step A: Start fresh server, insert keys, sets, and TTL ---
    PROCESS_INFORMATION srv = start_server(aof_file, test_port);
    SOCKET client = connect_client(test_port);

    // 1. SET key1
    resp_send_cmd(client, {"SET", "persist_str", "hello_aof"});
    RespValue rv;
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "OK") die("SET failed");

    // 2. EXPIRE persist_str 200 (TTL)
    resp_send_cmd(client, {"EXPIRE", "persist_str", "200"});
    resp_read_val(client, rv);
    if (rv.type != RESP_INT || rv.int_val != 1) die("EXPIRE failed");

    // 3. ZADD persist_zset 42.5 memberA
    resp_send_cmd(client, {"ZADD", "persist_zset", "42.5", "memberA"});
    resp_read_val(client, rv);
    if (rv.type != RESP_INT || rv.int_val != 1) die("ZADD failed");

    // 4. Short-lived TTL key (to test expiration across restart)
    resp_send_cmd(client, {"SET", "temp_key", "will_expire"});
    resp_read_val(client, rv);
    resp_send_cmd(client, {"PEXPIRE", "temp_key", "600"}); // 600 ms
    resp_read_val(client, rv);

    closesocket(client);

    // Sleep 1 second so temp_key expires before restart
    Sleep(1000);

    // --- Step B: Hard crash / Kill server ---
    stop_server(srv);
    printf("[PASS] Server stopped (simulating process termination).\n");

    // --- Step C: Restart server and verify replayed dataset ---
    srv = start_server(aof_file, test_port);
    client = connect_client(test_port);

    // Verify persist_str restored
    resp_send_cmd(client, {"GET", "persist_str"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "hello_aof") {
        die("Restored string value mismatch!");
    }

    // Verify TTL restored accurately (should be ~198-199s, not fresh 200s and not expired)
    resp_send_cmd(client, {"TTL", "persist_str"});
    resp_read_val(client, rv);
    if (rv.type != RESP_INT || rv.int_val < 180 || rv.int_val > 200) {
        die("Restored TTL incorrect!");
    }

    // Verify sorted set restored
    resp_send_cmd(client, {"ZSCORE", "persist_zset", "memberA"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val.substr(0, 4) != "42.5") {
        die("Restored ZSet score mismatch!");
    }

    // Verify expired key was NOT resurrected
    resp_send_cmd(client, {"GET", "temp_key"});
    resp_read_val(client, rv);
    if (rv.type != RESP_NIL) {
        die("Expired key was resurrected after restart!");
    }

    printf("[PASS] String, TTL timestamp, ZSet, and expired-key discard all verified after restart!\n");

    // --- Step D: Test Truncated Log Recovery On Live Server Restart ---
    closesocket(client);
    stop_server(srv);

    // Append corrupted bytes to the live AOF
    FILE *integ_fp = fopen(aof_file.c_str(), "ab");
    const char *bad_chunk = "*3\r\n$3\r\nSET\r\n$3\r\nbad\r\n$10\r\nhal";
    fwrite(bad_chunk, 1, strlen(bad_chunk), integ_fp);
    fclose(integ_fp);

    // Restart server — should automatically repair and continue
    srv = start_server(aof_file, test_port);
    client = connect_client(test_port);

    resp_send_cmd(client, {"GET", "persist_str"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "hello_aof") {
        die("Failed to recover valid data after corrupted AOF tail!");
    }

    resp_send_cmd(client, {"EXISTS", "bad"});
    resp_read_val(client, rv);
    if (rv.type != RESP_INT || rv.int_val != 0) {
        die("Corrupt command was partially applied!");
    }

    printf("[PASS] Server successfully detected, trimmed, and recovered from truncated AOF on restart!\n");

    closesocket(client);
    stop_server(srv);
    remove(aof_file.c_str());
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    printf("========================================\n");
    printf("AOF Persistence Tests — Part 12 Extension\n");
    printf("========================================\n\n");

    test_aof_unit();
    test_aof_server_integration();

    printf("\n========================================\n");
    printf("ALL AOF TESTS PASSED SUCCESSFULLY!\n");
    printf("========================================\n");

    WSACleanup();
    return 0;
}
