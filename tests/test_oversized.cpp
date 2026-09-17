// test_oversized.cpp — Sends a crafted message with length > k_max_msg to verify
// the server rejects it without crashing.
//
// This sends a raw 4-byte header claiming 9999 bytes, followed by only a few bytes
// of actual data. The server should:
//   1. Read the 4-byte header
//   2. See len=9999 > k_max_msg=4096
//   3. Log "message too long" and close the connection
//   4. Continue accepting new clients (not crash)
//
// Build: g++ -Wall -Wextra -std=c++14 -o test_oversized.exe tests/test_oversized.cpp -lws2_32
// Run:   test_oversized.exe    (with server.exe already running)

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("connect()");
    }

    printf("=== Test: Oversized message (len=9999, max=4096) ===\n");

    // Send a header claiming 9999 bytes
    uint32_t fake_len = 9999;
    char header[4];
    memcpy(header, &fake_len, 4);

    int sent = send(fd, header, 4, 0);
    if (sent == SOCKET_ERROR) {
        die("send() header");
    }
    printf("Sent header with len=%u\n", fake_len);

    // Send a few bytes of "payload" (doesn't matter what — the server should
    // reject based on the header alone, before reading any payload)
    const char *junk = "this should be rejected";
    send(fd, junk, (int)strlen(junk), 0);

    // Try to read a response — we expect the server to close the connection,
    // so recv() should return 0 (EOF) or SOCKET_ERROR.
    char buf[64];
    int n = recv(fd, buf, sizeof(buf), 0);
    if (n == 0) {
        printf("[PASS] Server closed the connection (rejected oversized message)\n");
    } else if (n == SOCKET_ERROR) {
        printf("[PASS] Server reset the connection (WSA error: %d)\n", WSAGetLastError());
    } else {
        printf("[FAIL] Server sent %d bytes back — it should have rejected!\n", n);
    }

    closesocket(fd);

    // ---- Now verify the server is still alive by sending a normal request ----
    printf("\n=== Verify: Server still alive after rejection ===\n");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) die("socket() #2");

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        printf("[FAIL] Cannot reconnect — server may have crashed!\n");
        closesocket(fd);
        WSACleanup();
        return 1;
    }

    // Send a normal "GET hello" message using list-of-strings format
    uint32_t msg_len = 4 + (4 + 3) + (4 + 5); // nstr + len(GET) + len(hello)
    memcpy(header, &msg_len, 4);
    send(fd, header, 4, 0);
    
    uint32_t nstr = 2;
    send(fd, (char*)&nstr, 4, 0);
    
    uint32_t l1 = 3;
    send(fd, (char*)&l1, 4, 0);
    send(fd, "GET", 3, 0);

    uint32_t l2 = 5;
    send(fd, (char*)&l2, 4, 0);
    send(fd, "hello", 5, 0);

    // Read response header
    char resp_header[4];
    int rv = recv(fd, resp_header, 4, 0);
    if (rv == 4) {
        uint32_t resp_len = 0;
        memcpy(&resp_len, resp_header, 4);

        // Read status code
        uint32_t status = 0;
        rv = recv(fd, (char*)&status, 4, 0);
        if (rv == 4) {
            uint32_t data_len = resp_len - 4;
            char resp_buf[64] = {0};
            if (data_len > 0) {
                rv = recv(fd, resp_buf, data_len, 0);
            } else {
                rv = 0;
            }
            if (rv == (int)data_len) {
                printf("[PASS] Server responded with status %u: \"%s\" — still alive!\n", status, resp_buf);
            } else {
                printf("[FAIL] Unexpected response payload length: %d (expected %d)\n", rv, data_len);
            }
        } else {
            printf("[FAIL] Could not read response status (got %d bytes)\n", rv);
        }
    } else {
        printf("[FAIL] Could not read response header (got %d bytes)\n", rv);
    }

    closesocket(fd);
    WSACleanup();
    return 0;
}
