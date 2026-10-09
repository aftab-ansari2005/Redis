// client.cpp — Part 6: Typed response deserialization and recursive pretty-printing.
//
// WHAT CHANGED:
//   The old client read [status:u32][raw bytes] and printed "status: N, data: <str>".
//   The new client reads a serialized typed value and pretty-prints it recursively.
//
//   This client is also your debugging tool going forward: any command's response,
//   no matter how deeply nested (arrays of arrays), will print readably.
//
// PRETTY-PRINT FORMAT:
//   (nil)                           — SER_NIL
//   (err) 1 ERR unknown command     — SER_ERR with code and message
//   (str) "hello world"             — SER_STR
//   (int) 42                        — SER_INT
//   (dbl) 3.140000                  — SER_DBL
//   (arr) len=2                     — SER_ARR header
//     [0] (str) "key1"              — array elements, indented
//     [1] (str) "key2"

#define WIN32_LEAN_AND_MEAN
// MinGW needs __USE_MINGW_ANSI_STDIO for %lld/%zu support in printf
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
#include "serialization.h"

static const size_t k_max_msg = 4096;

static void die(const char *msg) {
    fprintf(stderr, "[error] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

// --------------------------------------------------------------------------
// Blocking IO helpers (unchanged)
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
// send_request — encode and send a list-of-strings command (unchanged).
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

    char header[4];
    memcpy(header, &len, 4);
    if (write_full(fd, header, 4) != 0) {
        return -1;
    }

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
// Recursive pretty-printer for typed responses.
//
// This is the heart of the new client. It reads the type tag, then dispatches
// to the appropriate handler. For SER_ARR, it recurses for each element.
//
// Parameters:
//   data, len — cursor into the response payload (advanced as we consume)
//   depth     — nesting level for indentation
//
// Returns true on success, false if the data is malformed.
// --------------------------------------------------------------------------
static bool print_response(const uint8_t *&data, size_t &len, int depth) {
    // Print indentation for nested elements
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }

    uint8_t tag;
    if (!read_u8(data, len, tag)) {
        fprintf(stderr, "print_response: unexpected end of data (no type tag)\n");
        return false;
    }

    switch (tag) {
    case SER_NIL:
        printf("(nil)\n");
        return true;

    case SER_ERR: {
        uint32_t code;
        std::string msg;
        if (!read_u32(data, len, code)) return false;
        if (!read_str(data, len, msg)) return false;
        printf("(err) %u %s\n", code, msg.c_str());
        return true;
    }

    case SER_STR: {
        std::string val;
        if (!read_str(data, len, val)) return false;
        printf("(str) \"%s\"\n", val.c_str());
        return true;
    }

    case SER_INT: {
        int64_t val;
        if (!read_i64(data, len, val)) return false;
        printf("(int) %" PRId64 "\n", val);
        return true;
    }

    case SER_DBL: {
        double val;
        if (!read_f64(data, len, val)) return false;
        printf("(dbl) %f\n", val);
        return true;
    }

    case SER_ARR: {
        uint32_t count;
        if (!read_u32(data, len, count)) return false;
        printf("(arr) len=%u\n", count);
        for (uint32_t i = 0; i < count; i++) {
            // Print array index prefix at the element's depth
            for (int j = 0; j < depth + 1; j++) {
                printf("  ");
            }
            printf("[%u] ", i);
            // print_response will print its own indentation, but we've already
            // printed the prefix, so pass depth=0 for the element's own indent.
            // Actually, we want the element to NOT indent again, so we handle it
            // differently: we read the tag inline and dispatch.

            // Re-approach: let print_response handle it but with depth=0 since
            // we already printed the indent + index prefix.

            // Simplest correct approach: print_response prints the indent first,
            // so we need it to NOT print indent. Let's just restructure:
            // We already printed the indent + "[i] ", so now we need to parse
            // the value inline without extra indent.

            uint8_t elem_tag;
            if (!read_u8(data, len, elem_tag)) return false;

            switch (elem_tag) {
            case SER_NIL:
                printf("(nil)\n");
                break;
            case SER_ERR: {
                uint32_t code;
                std::string msg;
                if (!read_u32(data, len, code)) return false;
                if (!read_str(data, len, msg)) return false;
                printf("(err) %u %s\n", code, msg.c_str());
                break;
            }
            case SER_STR: {
                std::string val;
                if (!read_str(data, len, val)) return false;
                printf("(str) \"%s\"\n", val.c_str());
                break;
            }
            case SER_INT: {
                int64_t val;
                if (!read_i64(data, len, val)) return false;
                printf("(int) %" PRId64 "\n", val);
                break;
            }
            case SER_DBL: {
                double val;
                if (!read_f64(data, len, val)) return false;
                printf("(dbl) %f\n", val);
                break;
            }
            case SER_ARR: {
                // Nested array — recurse! Put back the tag and call print_response.
                // Actually we already consumed the tag, so handle inline:
                uint32_t sub_count;
                if (!read_u32(data, len, sub_count)) return false;
                printf("(arr) len=%u\n", sub_count);
                for (uint32_t j = 0; j < sub_count; j++) {
                    // Recurse at deeper indentation
                    if (!print_response(data, len, depth + 2)) return false;
                }
                break;
            }
            default:
                fprintf(stderr, "print_response: unknown type tag %u in array element\n", elem_tag);
                return false;
            }
        }
        return true;
    }

    default:
        fprintf(stderr, "print_response: unknown type tag: %u\n", tag);
        return false;
    }
}

// --------------------------------------------------------------------------
// read_response — read one response and pretty-print it.
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

    // Read the entire serialized payload
    std::vector<uint8_t> payload(len);
    if (len > 0) {
        if (read_full(fd, (char*)payload.data(), len) != 0) {
            fprintf(stderr, "read_response: failed to read payload\n");
            return -1;
        }
    }

    // Deserialize and pretty-print the typed value
    const uint8_t *data = payload.data();
    size_t remaining = len;
    if (!print_response(data, remaining, 0)) {
        fprintf(stderr, "read_response: failed to deserialize response\n");
        return -1;
    }

    if (remaining != 0) {
        fprintf(stderr, "read_response: %u trailing bytes after response\n", (unsigned)remaining);
    }

    return 0;
}

// --------------------------------------------------------------------------
// main
// --------------------------------------------------------------------------
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
