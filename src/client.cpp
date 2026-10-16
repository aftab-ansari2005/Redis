// client.cpp — Redis RESP2 client with recursive pretty-printing.

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
#include "resp.h"

static void die(const char *msg) {
    fprintf(stderr, "[error] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

// --------------------------------------------------------------------------
// Recursive pretty-printer for RESP2 responses
// --------------------------------------------------------------------------
static void print_resp_value(const RespValue &val, int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }

    switch (val.type) {
    case RESP_NIL:
        printf("(nil)\n");
        break;

    case RESP_ERR:
        printf("(err) %s\n", val.str_val.c_str());
        break;

    case RESP_STR:
        printf("(str) \"%s\"\n", val.str_val.c_str());
        break;

    case RESP_INT:
        printf("(int) %" PRId64 "\n", val.int_val);
        break;

    case RESP_ARR:
        printf("(arr) len=%u\n", (unsigned)val.arr_val.size());
        for (size_t i = 0; i < val.arr_val.size(); i++) {
            for (int j = 0; j < depth + 1; j++) {
                printf("  ");
            }
            printf("[%u] ", (unsigned)i);

            const RespValue &elem = val.arr_val[i];
            if (elem.type == RESP_ARR) {
                printf("(arr) len=%u\n", (unsigned)elem.arr_val.size());
                for (size_t k = 0; k < elem.arr_val.size(); k++) {
                    print_resp_value(elem.arr_val[k], depth + 2);
                }
            } else {
                switch (elem.type) {
                case RESP_NIL:
                    printf("(nil)\n");
                    break;
                case RESP_ERR:
                    printf("(err) %s\n", elem.str_val.c_str());
                    break;
                case RESP_STR:
                    printf("(str) \"%s\"\n", elem.str_val.c_str());
                    break;
                case RESP_INT:
                    printf("(int) %" PRId64 "\n", elem.int_val);
                    break;
                default:
                    printf("(unknown)\n");
                    break;
                }
            }
        }
        break;

    default:
        printf("(unknown)\n");
        break;
    }
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

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("connect()");
    }

    // Send the RESP command
    if (resp_send_cmd(fd, cmd) != 0) {
        die("resp_send_cmd()");
    }

    // Read and pretty-print the response
    RespValue resp;
    if (resp_read_val(fd, resp) != 0) {
        die("resp_read_val()");
    }

    print_resp_value(resp, 0);

    closesocket(fd);
    WSACleanup();
    return 0;
}
