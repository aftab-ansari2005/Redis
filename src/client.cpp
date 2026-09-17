// client.cpp — Part 2: Length-prefixed request/response client (Winsock2 / MinGW)
//
// Usage: client.exe <message>
//
// Sends the message as a length-prefixed frame:
//   [4-byte little-endian length] [payload bytes]
// Reads back one length-prefixed response and prints it.

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

// Response status codes
enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

static void die(const char *msg) {
    fprintf(stderr, "[error] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

// --------------------------------------------------------------------------
// read_full / write_full — identical to the server's versions.
//
// In a real project you'd put these in a shared utils.cpp and link both
// binaries against it. For now, duplicating ~20 lines keeps the build simple
// (one .cpp per binary, no linking headaches).
// --------------------------------------------------------------------------
static int read_full(SOCKET fd, char *buf, size_t n) {
    while (n > 0) {
        int rv = recv(fd, buf, (int)n, 0);
        if (rv == SOCKET_ERROR) {
            return -1;
        }
        if (rv == 0) {
            fprintf(stderr, "read_full: unexpected EOF\n");
            return -1;
        }
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

static int write_full(SOCKET fd, const char *buf, size_t n) {
    while (n > 0) {
        int rv = send(fd, buf, (int)n, 0);
        if (rv == SOCKET_ERROR) {
            return -1;
        }
        buf += rv;
        n -= (size_t)rv;
    }
    return 0;
}

// --------------------------------------------------------------------------
// send_request — encode and send a list-of-strings command.
//
// Wire format: [4 bytes: uint32 LE length]
//              [4 bytes: nstr]
//              [4 bytes: len1][str1]
//              [4 bytes: len2][str2] ...
// --------------------------------------------------------------------------
static int send_request(SOCKET fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + (uint32_t)s.size();
    }

    if (len > k_max_msg) {
        fprintf(stderr, "send_request: message too long (%u bytes, max %u)\n",
                len, (unsigned)k_max_msg);
        return -1;
    }

    // Send the 4-byte total length header
    char header[4];
    memcpy(header, &len, 4);
    if (write_full(fd, header, 4) != 0) {
        return -1;
    }

    // Send the payload
    uint32_t nstr = (uint32_t)cmd.size();
    if (write_full(fd, (char*)&nstr, 4) != 0) {
        return -1;
    }

    for (const std::string &s : cmd) {
        uint32_t slen = (uint32_t)s.size();
        if (write_full(fd, (char*)&slen, 4) != 0) {
            return -1;
        }
        if (write_full(fd, s.data(), slen) != 0) {
            return -1;
        }
    }

    return 0;
}

// --------------------------------------------------------------------------
// read_response — read one response and print status + data.
// --------------------------------------------------------------------------
static int read_response(SOCKET fd) {
    // Read the 4-byte total length header
    char header[4];
    if (read_full(fd, header, 4) != 0) {
        fprintf(stderr, "read_response: failed to read header\n");
        return -1;
    }

    uint32_t len = 0;
    memcpy(&len, header, 4);

    if (len > k_max_msg) {
        fprintf(stderr, "read_response: server response too long: %u bytes\n", len);
        return -1;
    }

    // Read the 4-byte status code
    uint32_t status = 0;
    if (read_full(fd, (char*)&status, 4) != 0) {
        fprintf(stderr, "read_response: failed to read status\n");
        return -1;
    }

    // Read the payload
    uint32_t data_len = len - 4;
    char buf[k_max_msg + 1];
    if (data_len > 0) {
        if (read_full(fd, buf, data_len) != 0) {
            fprintf(stderr, "read_response: failed to read payload\n");
            return -1;
        }
    }
    buf[data_len] = '\0';

    printf("Server response (status: %u): %s\n", status, buf);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: client.exe <cmd> [args...]\n");
        return 1;
    }

    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        die("socket()");
    }

    // Connect to server
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("connect()");
    }

    // Send the request and read the response
    if (send_request(fd, cmd) != 0) {
        die("send_request()");
    }
    if (read_response(fd) != 0) {
        die("read_response()");
    }

    closesocket(fd);
    WSACleanup();
    return 0;
}
