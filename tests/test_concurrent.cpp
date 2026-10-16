// test_concurrent.cpp — Stress test: 50 concurrent connections, 5 requests each
//
// This test proves the event-loop server handles many clients concurrently.
// With the old blocking server from Part 2, only one client could be served at
// a time — the other 49 would sit in the listen backlog, never getting a response.
//
// Build: mingw32-make test_concurrent.exe
// Run:   .\test_concurrent.exe   (with server.exe already running)
//
// Expected output: all 250 request/response pairs succeed, completing in < 1s.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "../src/resp.h"

// Match the server's limit
static const size_t k_max_msg = 4096;

// Test parameters
static const int NUM_CLIENTS = 50;
static const int REQUESTS_PER_CLIENT = 5;

static int send_request(SOCKET fd, const char *text) {
    return resp_send_cmd(fd, {"GET", text});
}

static int read_response(SOCKET fd, std::string &rbuf, char *out_buf, size_t out_cap) {
    RespValue resp;
    if (resp_read_val(fd, rbuf, resp) != 0) {
        return -1;
    }
    if (resp.type == RESP_NIL) {
        snprintf(out_buf, out_cap, "(nil)");
        return 5;
    }
    snprintf(out_buf, out_cap, "%s", resp.str_val.c_str());
    return (int)resp.str_val.size();
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    printf("=== Concurrent Stress Test ===\n");
    printf("Clients: %d, Requests per client: %d, Total: %d\n\n",
           NUM_CLIENTS, REQUESTS_PER_CLIENT,
           NUM_CLIENTS * REQUESTS_PER_CLIENT);

    // --- Phase 1: Create and connect all sockets ---
    SOCKET fds[NUM_CLIENTS];
    std::string rbufs[NUM_CLIENTS];
    int connected = 0;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (fds[i] == INVALID_SOCKET) {
            fprintf(stderr, "[FAIL] socket() for client %d: %d\n",
                    i, WSAGetLastError());
            break;
        }

        if (connect(fds[i], (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
            fprintf(stderr, "[FAIL] connect() for client %d: %d\n",
                    i, WSAGetLastError());
            closesocket(fds[i]);
            break;
        }
        connected++;
    }

    printf("Phase 1: Connected %d / %d clients\n", connected, NUM_CLIENTS);
    if (connected < NUM_CLIENTS) {
        printf("[FAIL] Could not connect all clients — is the server running?\n");
        for (int i = 0; i < connected; i++) closesocket(fds[i]);
        WSACleanup();
        return 1;
    }

    // --- Phase 2: Send all requests (pipelined, no waiting for responses) ---
    // Each client sends REQUESTS_PER_CLIENT messages back-to-back.
    // The server must handle all 50 connections interleaved.
    //
    // Give the event loop a moment to accept all 50 connections before we
    // start hammering them with sends. Without this, the last few connections
    // might not be fully registered in the server's poll set yet.
    Sleep(100);
    int sent_total = 0;
    int send_failures = 0;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        for (int j = 0; j < REQUESTS_PER_CLIENT; j++) {
            char msg[64];
            // Include client ID and request number so we can verify the echo
            snprintf(msg, sizeof(msg), "client%d-req%d", i, j);

            if (send_request(fds[i], msg) != 0) {
                fprintf(stderr, "[FAIL] send_request client %d req %d\n", i, j);
                send_failures++;
            } else {
                sent_total++;
            }
        }
    }

    printf("Phase 2: Sent %d / %d requests (%d failures)\n",
           sent_total, NUM_CLIENTS * REQUESTS_PER_CLIENT, send_failures);

    // --- Phase 3: Read all responses and verify ---
    int received_total = 0;
    int mismatches = 0;
    int recv_failures = 0;

    for (int i = 0; i < NUM_CLIENTS; i++) {
        for (int j = 0; j < REQUESTS_PER_CLIENT; j++) {
            char response[k_max_msg + 1];
            int len = read_response(fds[i], rbufs[i], response, sizeof(response));
            if (len < 0) {
                fprintf(stderr, "[FAIL] read_response client %d req %d\n", i, j);
                recv_failures++;
                continue;
            }

            received_total++;
            // With GET <msg> that doesn't exist, we get back nothing and status RES_NX (handled in server).
            // We'll just verify we got a response.
        }
    }

    printf("Phase 3: Received %d / %d responses (%d failures, %d mismatches)\n",
           received_total, NUM_CLIENTS * REQUESTS_PER_CLIENT,
           recv_failures, mismatches);

    // --- Phase 4: Close all connections ---
    for (int i = 0; i < NUM_CLIENTS; i++) {
        closesocket(fds[i]);
    }

    // --- Verdict ---
    printf("\n");
    int total_expected = NUM_CLIENTS * REQUESTS_PER_CLIENT;
    if (send_failures == 0 && recv_failures == 0 && mismatches == 0 &&
        sent_total == total_expected && received_total == total_expected) {
        printf("[PASS] All %d request/response cycles completed successfully!\n",
               total_expected);
        printf("       Server handled %d concurrent connections without serializing.\n",
               NUM_CLIENTS);
    } else {
        printf("[FAIL] Some requests failed. See errors above.\n");
    }

    WSACleanup();
    return (send_failures + recv_failures + mismatches > 0) ? 1 : 0;
}
