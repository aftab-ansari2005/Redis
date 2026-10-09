// test_timers.cpp — Chapter 12: Test idle connection timeouts
//
// Verifies:
// 1. An idle connection (no traffic) is automatically closed by the server after ~5 seconds.
// 2. An active connection that periodically sends requests remains connected and healthy.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include "../src/serialization.h"

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "[FAIL] Line %d: %s (WSA error: %d)\n", __LINE__, msg, WSAGetLastError()); \
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

static int read_response(SOCKET fd, std::vector<uint8_t> &out) {
    char header[4];
    if (read_full(fd, header, 4) != 0) return -1;
    uint32_t len = 0;
    memcpy(&len, header, 4);
    out.resize(len);
    if (read_full(fd, (char *)out.data(), len) != 0) return -1;
    return 0;
}

static SOCKET connect_client() {
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

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    printf("Connecting idle client (Conn 1)...\n");
    SOCKET idle_fd = connect_client();
    CHECK(idle_fd != INVALID_SOCKET, "Connect idle client failed (is server running?)");

    printf("Connecting active client (Conn 2)...\n");
    SOCKET active_fd = connect_client();
    CHECK(active_fd != INVALID_SOCKET, "Connect active client failed");

    // Send initial request on active client
    CHECK(send_request(active_fd, {"set", "timer_test", "hello"}) == 0, "Active client SET failed");
    std::vector<uint8_t> resp;
    CHECK(read_response(active_fd, resp) == 0, "Active client read response failed");
    printf("Conn 2: initial SET ok.\n");

    // Send keep-alive requests on Conn 2 every 1.5 seconds for 6 seconds
    // Meanwhile Conn 1 sends nothing.
    for (int i = 1; i <= 4; i++) {
        Sleep(1500);
        printf("Conn 2: heartbeat #%d at ~%.1f s...\n", i, i * 1.5);
        CHECK(send_request(active_fd, {"get", "timer_test"}) == 0, "Heartbeat GET failed");
        CHECK(read_response(active_fd, resp) == 0, "Heartbeat read response failed");
    }

    // At this point ~6.0 seconds have elapsed.
    // Conn 1 has been idle for 6.0 seconds (timeout is 5.0 seconds).
    // The server should have closed Conn 1.
    printf("Checking Conn 1 (idle client)...\n");
    char dummy[16];
    int rv = recv(idle_fd, dummy, sizeof(dummy), 0);
    printf("Conn 1 recv() returned %d (expected 0 for EOF/server close)\n", rv);
    CHECK(rv <= 0, "Idle client should have been closed by server (recv returned > 0)");

    // Conn 2 should still be completely functional
    printf("Checking Conn 2 (active client) is still alive...\n");
    CHECK(send_request(active_fd, {"get", "timer_test"}) == 0, "Final active client request failed");
    CHECK(read_response(active_fd, resp) == 0, "Final active client read response failed");
    printf("Conn 2: final request succeeded!\n");

    // Clean up test key
    send_request(active_fd, {"del", "timer_test"});
    read_response(active_fd, resp);

    closesocket(idle_fd);
    closesocket(active_fd);
    WSACleanup();

    printf("\n>>> Chapter 12 Timeout test PASSED! <<<\n");
    return 0;
}
