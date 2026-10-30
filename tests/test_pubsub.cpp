// test_pubsub.cpp — Comprehensive tests for SUBSCRIBE, UNSUBSCRIBE, PUBLISH, PSUBSCRIBE, PUNSUBSCRIBE, Subscribed Mode, and Slow Consumer Buffer Limit
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
    printf("Redis Pub/Sub Engine Verification Tests\n");
    printf("========================================\n\n");

    const std::string aof_file = "test_pubsub.aof";
    const int test_port = 12360;
    remove(aof_file.c_str());

    PROCESS_INFORMATION srv = start_server(aof_file, test_port);

    // ----------------------------------------------------------------------
    // Test 1: Basic SUBSCRIBE reply format
    // ----------------------------------------------------------------------
    printf("--- Test 1: SUBSCRIBE reply format ---\n");
    SOCKET sub1 = connect_client(test_port);
    std::string sub1_buf;
    RespValue rv;

    resp_send_cmd(sub1, {"SUBSCRIBE", "news", "sports"});
    
    // First channel confirmation
    if (resp_read_val(sub1, sub1_buf, rv) != 0) die("Failed to read first SUBSCRIBE reply");
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3) die("Expected 3-element array for SUBSCRIBE news");
    if (rv.arr_val[0].str_val != "subscribe" || rv.arr_val[1].str_val != "news" || rv.arr_val[2].int_val != 1) {
        die("Invalid reply content for SUBSCRIBE news");
    }

    // Second channel confirmation
    if (resp_read_val(sub1, sub1_buf, rv) != 0) die("Failed to read second SUBSCRIBE reply");
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3) die("Expected 3-element array for SUBSCRIBE sports");
    if (rv.arr_val[0].str_val != "subscribe" || rv.arr_val[1].str_val != "sports" || rv.arr_val[2].int_val != 2) {
        die("Invalid reply content for SUBSCRIBE sports");
    }
    printf("[PASS] SUBSCRIBE returned correct RESP2 push confirmation arrays.\n");

    // ----------------------------------------------------------------------
    // Test 2: Subscribed Mode command restrictions
    // ----------------------------------------------------------------------
    printf("\n--- Test 2: Subscribed Mode command restrictions ---\n");
    resp_send_cmd(sub1, {"GET", "foo"});
    resp_read_val(sub1, sub1_buf, rv);
    if (rv.type != RESP_ERR || rv.str_val.find("only (P)SUBSCRIBE") == std::string::npos) {
        die("Expected restriction error on GET while subscribed");
    }

    resp_send_cmd(sub1, {"SET", "k", "v"});
    resp_read_val(sub1, sub1_buf, rv);
    if (rv.type != RESP_ERR || rv.str_val.find("only (P)SUBSCRIBE") == std::string::npos) {
        die("Expected restriction error on SET while subscribed");
    }

    // PING in subscribed mode returns ["pong", ""]
    resp_send_cmd(sub1, {"PING"});
    resp_read_val(sub1, sub1_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 2 || rv.arr_val[0].str_val != "pong") {
        die("PING in subscribed mode must return ['pong', ''] array");
    }

    // PING with argument returns ["pong", arg]
    resp_send_cmd(sub1, {"PING", "hello"});
    resp_read_val(sub1, sub1_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 2 || rv.arr_val[0].str_val != "pong" || rv.arr_val[1].str_val != "hello") {
        die("PING with arg in subscribed mode must return ['pong', 'hello']");
    }
    printf("[PASS] Subscribed mode correctly rejects arbitrary commands and responds to PING.\n");

    // ----------------------------------------------------------------------
    // Test 3: Fan-out to multiple subscribers (Requirement 8 test flow)
    // ----------------------------------------------------------------------
    printf("\n--- Test 3: Fan-out to multiple subscribers & PUBLISH count ---\n");
    SOCKET sub2 = connect_client(test_port);
    std::string sub2_buf;
    resp_send_cmd(sub2, {"SUBSCRIBE", "news"});
    resp_read_val(sub2, sub2_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val[2].int_val != 1) die("sub2 SUBSCRIBE news failed");

    SOCKET pub = connect_client(test_port);
    std::string pub_buf;
    resp_send_cmd(pub, {"PUBLISH", "news", "hello subscribers"});
    resp_read_val(pub, pub_buf, rv);
    if (rv.type != RESP_INT || rv.int_val != 2) {
        fprintf(stderr, "Expected PUBLISH receiver count = 2, got %s\n", std::to_string(rv.int_val).c_str());
        die("Incorrect receiver count for PUBLISH news");
    }

    // Check message delivery on sub1
    resp_read_val(sub1, sub1_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3 ||
        rv.arr_val[0].str_val != "message" ||
        rv.arr_val[1].str_val != "news" ||
        rv.arr_val[2].str_val != "hello subscribers") {
        die("Sub1 did not receive published message correctly");
    }

    // Check message delivery on sub2
    resp_read_val(sub2, sub2_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3 ||
        rv.arr_val[0].str_val != "message" ||
        rv.arr_val[1].str_val != "news" ||
        rv.arr_val[2].str_val != "hello subscribers") {
        die("Sub2 did not receive published message correctly");
    }
    printf("[PASS] PUBLISH returned 2 and pushed message array delivered to both subscribers.\n");

    // ----------------------------------------------------------------------
    // Test 4: Disconnect one subscriber and verify cleanup & no crash
    // ----------------------------------------------------------------------
    printf("\n--- Test 4: Disconnect resilience & channel garbage collection ---\n");
    closesocket(sub1);
    Sleep(100); // Give server event loop a moment to detect and sweep

    resp_send_cmd(pub, {"PUBLISH", "news", "hello again"});
    resp_read_val(pub, pub_buf, rv);
    if (rv.type != RESP_INT || rv.int_val != 1) {
        fprintf(stderr, "Expected PUBLISH receiver count = 1 after killing sub1, got %s\n", std::to_string(rv.int_val).c_str());
        die("Incorrect receiver count after sub1 disconnect");
    }

    resp_read_val(sub2, sub2_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val[2].str_val != "hello again") {
        die("Sub2 failed to receive second message after sub1 disconnect");
    }
    printf("[PASS] Disconnected subscriber cleaned up cleanly; PUBLISH returned 1 with no crash.\n");

    // ----------------------------------------------------------------------
    // Test 5: UNSUBSCRIBE and exit Subscribed Mode
    // ----------------------------------------------------------------------
    printf("\n--- Test 5: UNSUBSCRIBE & exit from subscribed mode ---\n");
    resp_send_cmd(sub2, {"UNSUBSCRIBE", "news"});
    resp_read_val(sub2, sub2_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3 ||
        rv.arr_val[0].str_val != "unsubscribe" ||
        rv.arr_val[1].str_val != "news" ||
        rv.arr_val[2].int_val != 0) {
        die("UNSUBSCRIBE reply format mismatch");
    }

    // sub2 is now no longer subscribed, so SET should succeed!
    resp_send_cmd(sub2, {"SET", "normal_key", "val"});
    resp_read_val(sub2, sub2_buf, rv);
    if (rv.type != RESP_STR || rv.str_val != "OK") {
        die("SET failed after unsubscription; connection failed to exit subscribed mode");
    }

    resp_send_cmd(pub, {"PUBLISH", "news", "nobody listening"});
    resp_read_val(pub, pub_buf, rv);
    if (rv.type != RESP_INT || rv.int_val != 0) {
        die("Expected 0 receivers after unsubscription");
    }
    printf("[PASS] UNSUBSCRIBE succeeded and connection successfully returned to normal mode.\n");

    // ----------------------------------------------------------------------
    // Test 6: Pattern Matching (PSUBSCRIBE / PUNSUBSCRIBE / PMESSAGE)
    // ----------------------------------------------------------------------
    printf("\n--- Test 6: PSUBSCRIBE / PMESSAGE / PUNSUBSCRIBE ---\n");
    SOCKET psub = connect_client(test_port);
    std::string psub_buf;
    resp_send_cmd(psub, {"PSUBSCRIBE", "sensor.*"});
    resp_read_val(psub, psub_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3 ||
        rv.arr_val[0].str_val != "psubscribe" ||
        rv.arr_val[1].str_val != "sensor.*" ||
        rv.arr_val[2].int_val != 1) {
        die("PSUBSCRIBE confirmation failed");
    }

    resp_send_cmd(pub, {"PUBLISH", "sensor.temp", "24.5C"});
    resp_read_val(pub, pub_buf, rv);
    if (rv.type != RESP_INT || rv.int_val != 1) die("Pattern publish receiver count mismatch");

    resp_read_val(psub, psub_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 4 ||
        rv.arr_val[0].str_val != "pmessage" ||
        rv.arr_val[1].str_val != "sensor.*" ||
        rv.arr_val[2].str_val != "sensor.temp" ||
        rv.arr_val[3].str_val != "24.5C") {
        die("PMESSAGE delivery format mismatch");
    }

    resp_send_cmd(psub, {"PUNSUBSCRIBE", "sensor.*"});
    resp_read_val(psub, psub_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val.size() != 3 ||
        rv.arr_val[0].str_val != "punsubscribe" ||
        rv.arr_val[1].str_val != "sensor.*" ||
        rv.arr_val[2].int_val != 0) {
        die("PUNSUBSCRIBE confirmation failed");
    }
    closesocket(psub);
    printf("[PASS] Glob pattern matching and PMESSAGE notifications verified.\n");

    // ----------------------------------------------------------------------
    // Test 7: Slow Subscriber Protection (Buffer Limit Exceeded)
    // ----------------------------------------------------------------------
    printf("\n--- Test 7: Slow subscriber buffer limit disconnection ---\n");
    SOCKET slow = connect_client(test_port);
    std::string slow_buf;
    resp_send_cmd(slow, {"SUBSCRIBE", "flood"});
    resp_read_val(slow, slow_buf, rv);
    if (rv.type != RESP_ARR || rv.arr_val[2].int_val != 1) die("slow sub subscribe failed");

    // Set a tiny receive buffer on client socket so OS stops reading from TCP stream
    int rcvbuf_sz = 1024;
    setsockopt(slow, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf_sz, sizeof(rcvbuf_sz));

    // Flood with messages: 150 * 32 KB = 4.8 MB > 4 MB limit
    std::string big_payload(32768, 'A');
    bool slow_disconnected = false;

    for (int i = 0; i < 200; i++) {
        resp_send_cmd(pub, {"PUBLISH", "flood", big_payload});
        resp_read_val(pub, pub_buf, rv);

        // Check if slow socket dropped (do NOT consume from buffer)
        char check_buf[16];
        int r = recv(slow, check_buf, sizeof(check_buf), MSG_PEEK);
        if (r == 0 || (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
            slow_disconnected = true;
            printf("[INFO] Slow subscriber terminated by server at iteration %d (buffer limit enforced).\n", i);
            break;
        }
        Sleep(2);
    }

    if (!slow_disconnected) {
        // Final check: drain whatever came in, then check if socket closed
        u_long mode = 1;
        ioctlsocket(slow, FIONBIO, &mode);
        char drain[65536];
        while (recv(slow, drain, sizeof(drain), 0) > 0) {}
        int r = recv(slow, drain, 1, 0);
        if (r == 0 || (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
            slow_disconnected = true;
        }
    }

    if (!slow_disconnected) {
        die("Server failed to disconnect slow subscriber exceeding buffer limit");
    }

    // Verify publisher is still fully operational
    resp_send_cmd(pub, {"PING"});
    resp_read_val(pub, pub_buf, rv);
    if (rv.type != RESP_STR || rv.str_val != "PONG") die("Server became unresponsive after dropping slow client");

    printf("[PASS] Slow subscriber disconnected when buffer limit exceeded; server remains healthy.\n");

    closesocket(slow);
    closesocket(pub);
    closesocket(sub2);
    stop_server(srv);
    remove(aof_file.c_str());

    printf("\n========================================\n");
    printf("ALL PUB/SUB TESTS PASSED!\n");
    printf("========================================\n");
    return 0;
}
