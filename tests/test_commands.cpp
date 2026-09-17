// test_commands.cpp — Verify GET, SET, DEL functionality
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>

static const size_t k_max_msg = 4096;

enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

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

static int read_response(SOCKET fd, uint32_t &out_status, std::string &out_data) {
    char header[4];
    if (read_full(fd, header, 4) != 0) return -1;

    uint32_t len = 0;
    memcpy(&len, header, 4);
    if (len > k_max_msg || len < 4) return -1;

    if (read_full(fd, (char*)&out_status, 4) != 0) return -1;

    uint32_t data_len = len - 4;
    if (data_len > 0) {
        std::vector<char> buf(data_len);
        if (read_full(fd, buf.data(), data_len) != 0) return -1;
        out_data.assign(buf.data(), data_len);
    } else {
        out_data.clear();
    }
    return 0;
}

static int run_test(SOCKET fd, const std::vector<std::string> &cmd, uint32_t expected_status, const std::string &expected_data) {
    if (send_request(fd, cmd) != 0) {
        printf("[FAIL] Failed to send request\n");
        return 1;
    }

    uint32_t status;
    std::string data;
    if (read_response(fd, status, data) != 0) {
        printf("[FAIL] Failed to read response\n");
        return 1;
    }

    if (status != expected_status) {
        printf("[FAIL] Expected status %u, got %u\n", expected_status, status);
        return 1;
    }

    if (data != expected_data) {
        printf("[FAIL] Expected data '%s', got '%s'\n", expected_data.c_str(), data.c_str());
        return 1;
    }
    
    printf("[PASS] command ");
    for(const auto& s : cmd) printf("%s ", s.c_str());
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

    // Test 1: GET non-existent key
    failures += run_test(fd, {"GET", "key1"}, RES_NX, "");

    // Test 2: SET key
    failures += run_test(fd, {"SET", "key1", "val1"}, RES_OK, "OK");

    // Test 3: GET existing key
    failures += run_test(fd, {"GET", "key1"}, RES_OK, "val1");

    // Test 4: DEL key
    failures += run_test(fd, {"DEL", "key1"}, RES_OK, "1");

    // Test 5: GET deleted key
    failures += run_test(fd, {"GET", "key1"}, RES_NX, "");

    // Test 6: DEL non-existent key
    failures += run_test(fd, {"DEL", "key1"}, RES_OK, "0");

    // Test 7: Invalid command
    failures += run_test(fd, {"FOO"}, RES_ERR, "ERR unknown command");

    // Test 8: SET wrong arguments
    failures += run_test(fd, {"SET", "k"}, RES_ERR, "ERR wrong number of arguments for 'set' command");

    // Test 9: EXISTS on non-existent key
    failures += run_test(fd, {"EXISTS", "nokey"}, RES_OK, "0");

    // Test 10: SET then EXISTS
    failures += run_test(fd, {"SET", "ekey", "eval"}, RES_OK, "OK");
    failures += run_test(fd, {"EXISTS", "ekey"}, RES_OK, "1");

    // Test 11: DEL then EXISTS
    failures += run_test(fd, {"DEL", "ekey"}, RES_OK, "1");
    failures += run_test(fd, {"EXISTS", "ekey"}, RES_OK, "0");

    // Test 12: EXISTS wrong number of arguments
    failures += run_test(fd, {"EXISTS"}, RES_ERR, "ERR wrong number of arguments for 'exists' command");

    if (failures == 0) {
        printf("\nAll KV commands tests passed!\n");
    } else {
        printf("\n%d tests failed.\n", failures);
    }

    closesocket(fd);
    WSACleanup();
    return failures;
}
