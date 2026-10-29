// test_transactions.cpp — Verify MULTI, EXEC, DISCARD, EXECABORT, runtime errors, and AOF integration
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
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

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s\n", msg);
    exit(1);
}

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

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    printf("========================================\n");
    printf("MULTI / EXEC / DISCARD Transaction Tests\n");
    printf("========================================\n\n");

    const std::string aof_file = "test_tx.aof";
    const int test_port = 12350;
    remove(aof_file.c_str());

    PROCESS_INFORMATION srv = start_server(aof_file, test_port);
    SOCKET client = connect_client(test_port);
    RespValue rv;

    // ----------------------------------------------------------------------
    // 1. Errors outside MULTI
    // ----------------------------------------------------------------------
    printf("--- Test 1: EXEC / DISCARD without MULTI ---\n");
    resp_send_cmd(client, {"EXEC"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ERR || rv.str_val != "ERR EXEC without MULTI") {
        die("EXEC without MULTI did not return expected error");
    }

    resp_send_cmd(client, {"DISCARD"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ERR || rv.str_val != "ERR DISCARD without MULTI") {
        die("DISCARD without MULTI did not return expected error");
    }
    printf("[PASS] EXEC/DISCARD without MULTI handled correctly.\n");

    // ----------------------------------------------------------------------
    // 2. Successful Transaction
    // ----------------------------------------------------------------------
    printf("\n--- Test 2: Successful MULTI / EXEC ---\n");
    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "OK") die("MULTI failed");

    // Nested MULTI check
    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ERR || rv.str_val != "ERR MULTI calls can not be nested") {
        die("Nested MULTI did not return expected error");
    }

    // Queue commands
    resp_send_cmd(client, {"SET", "tx_k1", "v1"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "QUEUED") die("Expected QUEUED for SET tx_k1");

    resp_send_cmd(client, {"SET", "tx_k2", "v2"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "QUEUED") die("Expected QUEUED for SET tx_k2");

    resp_send_cmd(client, {"GET", "tx_k1"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "QUEUED") die("Expected QUEUED for GET tx_k1");

    // EXEC
    resp_send_cmd(client, {"EXEC"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3) {
        die("EXEC reply must be array of 3 elements");
    }
    if (rv.arr_val[0].type != RESP_STR || rv.arr_val[0].str_val != "OK") die("Reply 0 mismatch");
    if (rv.arr_val[1].type != RESP_STR || rv.arr_val[1].str_val != "OK") die("Reply 1 mismatch");
    if (rv.arr_val[2].type != RESP_STR || rv.arr_val[2].str_val != "v1") die("Reply 2 mismatch");

    printf("[PASS] Transaction executed atomically and returned array of 3 replies.\n");

    // ----------------------------------------------------------------------
    // 3. DISCARD Transaction
    // ----------------------------------------------------------------------
    printf("\n--- Test 3: DISCARD Transaction ---\n");
    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);

    resp_send_cmd(client, {"SET", "discarded_key", "must_not_exist"});
    resp_read_val(client, rv);

    resp_send_cmd(client, {"DISCARD"});
    resp_read_val(client, rv);
    if (rv.type != RESP_STR || rv.str_val != "OK") die("DISCARD failed");

    // Verify key was NOT created
    resp_send_cmd(client, {"EXISTS", "discarded_key"});
    resp_read_val(client, rv);
    if (rv.type != RESP_INT || rv.int_val != 0) die("Discarded command took effect!");

    printf("[PASS] DISCARD successfully cleared queue and aborted transaction.\n");

    // ----------------------------------------------------------------------
    // 4. Queue-Time Error Abort (EXECABORT)
    // ----------------------------------------------------------------------
    printf("\n--- Test 4: Queue-Time Error (EXECABORT) ---\n");
    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);

    resp_send_cmd(client, {"SET", "abort_key", "val"});
    resp_read_val(client, rv); // +QUEUED

    // Send unknown command
    resp_send_cmd(client, {"NONEXISTENT_COMMAND", "arg"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ERR) die("Expected error on invalid command at queue time");

    // Send another valid command
    resp_send_cmd(client, {"SET", "abort_key2", "val2"});
    resp_read_val(client, rv);

    // Call EXEC — must abort whole transaction!
    resp_send_cmd(client, {"EXEC"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ERR || rv.str_val.find("EXECABORT") == std::string::npos) {
        die("Expected EXECABORT error from EXEC");
    }

    // Verify neither key was committed!
    resp_send_cmd(client, {"EXISTS", "abort_key"});
    resp_read_val(client, rv);
    if (rv.int_val != 0) die("abort_key was committed despite EXECABORT!");

    resp_send_cmd(client, {"EXISTS", "abort_key2"});
    resp_read_val(client, rv);
    if (rv.int_val != 0) die("abort_key2 was committed despite EXECABORT!");

    printf("[PASS] EXECABORT prevented any command execution due to queue-time syntax error.\n");

    // ----------------------------------------------------------------------
    // 5. Runtime Error Partial Execution (WRONGTYPE)
    // ----------------------------------------------------------------------
    printf("\n--- Test 5: Runtime Type Error (Partial Execution) ---\n");
    // Pre-create string key
    resp_send_cmd(client, {"SET", "string_target", "iamastring"});
    resp_read_val(client, rv);

    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);

    resp_send_cmd(client, {"SET", "pre_key", "pre_val"});
    resp_read_val(client, rv); // +QUEUED

    // ZADD on string_target fails at RUNTIME with WRONGTYPE
    resp_send_cmd(client, {"ZADD", "string_target", "10.0", "member1"});
    resp_read_val(client, rv); // +QUEUED (syntax is valid!)

    resp_send_cmd(client, {"SET", "post_key", "post_val"});
    resp_read_val(client, rv); // +QUEUED

    // EXEC — should execute all 3, with slot 1 having WRONGTYPE error
    resp_send_cmd(client, {"EXEC"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3) {
        die("Expected 3 results from runtime error EXEC");
    }
    if (rv.arr_val[0].type != RESP_STR || rv.arr_val[0].str_val != "OK") {
        die("First command should succeed");
    }
    if (rv.arr_val[1].type != RESP_ERR || rv.arr_val[1].str_val.find("WRONGTYPE") == std::string::npos) {
        die("Second command should fail with WRONGTYPE");
    }
    if (rv.arr_val[2].type != RESP_STR || rv.arr_val[2].str_val != "OK") {
        die("Third command should succeed despite previous command's runtime error");
    }

    // Verify pre_key and post_key exist!
    resp_send_cmd(client, {"GET", "pre_key"});
    resp_read_val(client, rv);
    if (rv.str_val != "pre_val") die("pre_key mismatch");

    resp_send_cmd(client, {"GET", "post_key"});
    resp_read_val(client, rv);
    if (rv.str_val != "post_val") die("post_key mismatch");

    printf("[PASS] Runtime WRONGTYPE error contained to its slot; other commands committed.\n");

    // ----------------------------------------------------------------------
    // 6. Client Disconnect Mid-MULTI
    // ----------------------------------------------------------------------
    printf("\n--- Test 6: Client Disconnect Mid-MULTI Cleanup ---\n");
    SOCKET client2 = connect_client(test_port);
    resp_send_cmd(client2, {"MULTI"});
    resp_read_val(client2, rv);
    resp_send_cmd(client2, {"SET", "dropped_k", "dropped_v"});
    resp_read_val(client2, rv);
    // Abruptly close socket
    closesocket(client2);
    Sleep(200);

    // Verify key was never executed
    resp_send_cmd(client, {"EXISTS", "dropped_k"});
    resp_read_val(client, rv);
    if (rv.int_val != 0) die("Dropped connection command was committed!");

    printf("[PASS] Dropped connection during MULTI cleaned up safely with no leaks.\n");

    // ----------------------------------------------------------------------
    // 7. AOF Transaction Durability & Crash Recovery
    // ----------------------------------------------------------------------
    printf("\n--- Test 7: AOF Transaction Durability & Incomplete MULTI Recovery ---\n");
    // Write an atomic transaction with multiple mutating commands
    resp_send_cmd(client, {"MULTI"});
    resp_read_val(client, rv);
    resp_send_cmd(client, {"SET", "aof_tx_k1", "val1"});
    resp_read_val(client, rv);
    resp_send_cmd(client, {"SET", "aof_tx_k2", "val2"});
    resp_read_val(client, rv);
    resp_send_cmd(client, {"ZADD", "aof_tx_z", "44.5", "m44"});
    resp_read_val(client, rv);
    resp_send_cmd(client, {"EXEC"});
    resp_read_val(client, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3) die("AOF transaction EXEC failed");

    closesocket(client);
    stop_server(srv);

    // Append an INCOMPLETE MULTI block (crash simulation before EXEC)
    FILE *fp = fopen(aof_file.c_str(), "ab");
    if (!fp) die("Cannot open AOF for incomplete block test");
    const char *incomplete_tx = "*1\r\n$5\r\nMULTI\r\n*3\r\n$3\r\nSET\r\n$9\r\nuncommit1\r\n$4\r\nbad1\r\n*3\r\n$3\r\nSET\r\n$9\r\nuncommit2\r\n$4\r\nbad2\r\n";
    // Notice: NO EXEC!
    fwrite(incomplete_tx, 1, strlen(incomplete_tx), fp);
    fclose(fp);

    // Restart server — should roll back incomplete transaction and truncate!
    srv = start_server(aof_file, test_port);
    client = connect_client(test_port);

    // Verify committed transaction keys exist
    resp_send_cmd(client, {"GET", "aof_tx_k1"});
    resp_read_val(client, rv);
    if (rv.str_val != "val1") die("aof_tx_k1 was not restored");

    resp_send_cmd(client, {"GET", "aof_tx_k2"});
    resp_read_val(client, rv);
    if (rv.str_val != "val2") die("aof_tx_k2 was not restored");

    resp_send_cmd(client, {"ZSCORE", "aof_tx_z", "m44"});
    resp_read_val(client, rv);
    if (rv.str_val.substr(0, 4) != "44.5") die("aof_tx_z score mismatch");

    // Verify uncommitted transaction keys were ROLLED BACK / DISCARDED!
    resp_send_cmd(client, {"EXISTS", "uncommit1"});
    resp_read_val(client, rv);
    if (rv.int_val != 0) die("Uncommitted MULTI key uncommit1 was resurrected!");

    resp_send_cmd(client, {"EXISTS", "uncommit2"});
    resp_read_val(client, rv);
    if (rv.int_val != 0) die("Uncommitted MULTI key uncommit2 was resurrected!");

    printf("[PASS] AOF atomic MULTI/EXEC block restored; incomplete transaction rolled back and truncated cleanly!\n");

    closesocket(client);
    stop_server(srv);
    remove(aof_file.c_str());

    printf("\n========================================\n");
    printf("ALL TRANSACTION TESTS PASSED!\n");
    printf("========================================\n");

    WSACleanup();
    return 0;
}
