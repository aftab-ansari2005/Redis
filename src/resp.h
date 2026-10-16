// resp.h — Redis RESP2 protocol serializer and incremental parser
#ifndef RESP_H
#define RESP_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// --------------------------------------------------------------------------
// Safety Limits
// --------------------------------------------------------------------------
static const size_t k_max_bulk_len  = 64 * 1024 * 1024; // 64 MB
static const size_t k_max_array_len = 1024 * 1024;      // 1M elements
static const size_t k_max_inline_len= 64 * 1024;        // 64 KB inline command

// --------------------------------------------------------------------------
// RESP2 Serializers
// --------------------------------------------------------------------------

// Simple string: +<str>\r\n (e.g. +OK\r\n, +PONG\r\n)
static inline void resp_out_str(std::string &out, const std::string &s) {
    out.push_back('+');
    out.append(s);
    out.append("\r\n");
}

// Error: -<msg>\r\n (e.g. -ERR unknown command\r\n)
static inline void resp_out_err(std::string &out, const std::string &msg) {
    out.push_back('-');
    out.append(msg);
    out.append("\r\n");
}

// Integer: :<val>\r\n (e.g. :1\r\n, :0\r\n)
static inline void resp_out_int(std::string &out, int64_t val) {
    out.push_back(':');
    out.append(std::to_string(val));
    out.append("\r\n");
}

// Bulk string: $<len>\r\n<val>\r\n
static inline void resp_out_bulk(std::string &out, const std::string &val) {
    out.push_back('$');
    out.append(std::to_string(val.size()));
    out.append("\r\n");
    out.append(val);
    out.append("\r\n");
}

static inline void resp_out_bulk(std::string &out, const char *val, size_t len) {
    out.push_back('$');
    out.append(std::to_string(len));
    out.append("\r\n");
    out.append(val, len);
    out.append("\r\n");
}

// Null bulk string: $-1\r\n
static inline void resp_out_null_bulk(std::string &out) {
    out.append("$-1\r\n");
}

// Array header: *<count>\r\n
static inline void resp_out_arr_header(std::string &out, size_t count) {
    out.push_back('*');
    out.append(std::to_string(count));
    out.append("\r\n");
}

// Null array: *-1\r\n
static inline void resp_out_null_arr(std::string &out) {
    out.append("*-1\r\n");
}

// --------------------------------------------------------------------------
// Request Parser (Incremental)
// --------------------------------------------------------------------------

enum ParseStatus {
    PARSE_OK = 0,
    PARSE_NEED_MORE = 1,
    PARSE_ERR = 2,
};

// Scan for "\r\n" in a memory buffer without requiring GNU memmem
static inline const uint8_t *resp_find_crlf(const uint8_t *data, size_t len) {
    if (len < 2) return NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n') {
            return data + i;
        }
    }
    return NULL;
}

// Parse a single request from `data` of length `len`.
// Returns:
//   PARSE_OK: exactly one command extracted into `cmd`, `consumed` set to bytes used.
//   PARSE_NEED_MORE: buffer incomplete, `consumed` NOT modified, do not alter buffer!
//   PARSE_ERR: protocol violation, malformed data, or bounds exceeded.
static inline ParseStatus parse_request(
    const uint8_t *data, size_t len,
    size_t &consumed, std::vector<std::string> &cmd)
{
    if (len == 0) return PARSE_NEED_MORE;

    // --- Case 1: Inline command (e.g. PING\r\n, SET k v\r\n) ---
    if (data[0] != '*') {
        const uint8_t *nl = (const uint8_t *)memchr(data, '\n', len);
        if (!nl) {
            if (len > k_max_inline_len) {
                return PARSE_ERR;
            }
            return PARSE_NEED_MORE;
        }
        size_t line_len = (size_t)(nl - data) + 1;
        consumed = line_len;

        // Strip trailing \r and \n
        size_t content_len = line_len;
        while (content_len > 0 && (data[content_len - 1] == '\r' || data[content_len - 1] == '\n')) {
            content_len--;
        }

        // Tokenize command by spaces or tabs
        std::string line((const char *)data, content_len);
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
            if (i >= line.size()) break;
            size_t start = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') i++;
            cmd.push_back(line.substr(start, i - start));
        }
        return PARSE_OK;
    }

    // --- Case 2: Standard RESP Array of Bulk Strings (*<count>\r\n...) ---
    size_t pos = 1; // skip leading '*'
    const uint8_t *crlf = resp_find_crlf(data + pos, len - pos);
    if (!crlf) return PARSE_NEED_MORE;

    std::string count_str((const char *)(data + pos), crlf - (data + pos));
    pos = (size_t)(crlf - data) + 2;

    long long count = 0;
    try {
        count = std::stoll(count_str);
    } catch (...) {
        return PARSE_ERR;
    }

    if (count < 0 || (size_t)count > k_max_array_len) {
        return PARSE_ERR;
    }

    for (long long i = 0; i < count; i++) {
        if (pos >= len) return PARSE_NEED_MORE;
        if (data[pos] != '$') return PARSE_ERR;
        pos++;

        crlf = resp_find_crlf(data + pos, len - pos);
        if (!crlf) return PARSE_NEED_MORE;

        std::string blen_str((const char *)(data + pos), crlf - (data + pos));
        pos = (size_t)(crlf - data) + 2;

        long long blen = 0;
        try {
            blen = std::stoll(blen_str);
        } catch (...) {
            return PARSE_ERR;
        }

        if (blen < 0 || (size_t)blen > k_max_bulk_len) {
            return PARSE_ERR;
        }

        // Check if payload and trailing \r\n are present in buffer
        if (pos + (size_t)blen + 2 > len) {
            return PARSE_NEED_MORE;
        }

        // Validate trailing \r\n
        if (data[pos + (size_t)blen] != '\r' || data[pos + (size_t)blen + 1] != '\n') {
            return PARSE_ERR;
        }

        cmd.push_back(std::string((const char *)(data + pos), (size_t)blen));
        pos += (size_t)blen + 2;
    }

    consumed = pos;
    return PARSE_OK;
}

// --------------------------------------------------------------------------
// Client RESP Deserialization & Socket IO Helpers
// --------------------------------------------------------------------------

enum RespType {
    RESP_NIL = 0,
    RESP_ERR = 1,
    RESP_STR = 2,
    RESP_INT = 3,
    RESP_ARR = 4,
};

struct RespValue {
    RespType type = RESP_NIL;
    std::string str_val;            // for RESP_STR (simple or bulk) and RESP_ERR
    int64_t int_val = 0;            // for RESP_INT
    std::vector<RespValue> arr_val; // for RESP_ARR
};

// Parse a typed RESP response value from [data, len]
static inline ParseStatus parse_resp_value(
    const uint8_t *data, size_t len,
    size_t &consumed, RespValue &out)
{
    if (len == 0) return PARSE_NEED_MORE;

    char tag = (char)data[0];
    if (tag == '+' || tag == '-') {
        const uint8_t *crlf = resp_find_crlf(data + 1, len - 1);
        if (!crlf) return PARSE_NEED_MORE;
        out.type = (tag == '+') ? RESP_STR : RESP_ERR;
        out.str_val.assign((const char *)(data + 1), (size_t)(crlf - (data + 1)));
        consumed = (size_t)(crlf - data) + 2;
        return PARSE_OK;
    }
    else if (tag == ':') {
        const uint8_t *crlf = resp_find_crlf(data + 1, len - 1);
        if (!crlf) return PARSE_NEED_MORE;
        std::string s((const char *)(data + 1), (size_t)(crlf - (data + 1)));
        try {
            out.int_val = std::stoll(s);
        } catch (...) {
            return PARSE_ERR;
        }
        out.type = RESP_INT;
        consumed = (size_t)(crlf - data) + 2;
        return PARSE_OK;
    }
    else if (tag == '$') {
        const uint8_t *crlf = resp_find_crlf(data + 1, len - 1);
        if (!crlf) return PARSE_NEED_MORE;
        std::string slen((const char *)(data + 1), (size_t)(crlf - (data + 1)));
        long long blen = 0;
        try {
            blen = std::stoll(slen);
        } catch (...) {
            return PARSE_ERR;
        }
        size_t header_len = (size_t)(crlf - data) + 2;
        if (blen == -1) {
            out.type = RESP_NIL;
            consumed = header_len;
            return PARSE_OK;
        }
        if (blen < 0 || (size_t)blen > k_max_bulk_len) return PARSE_ERR;
        if (header_len + (size_t)blen + 2 > len) return PARSE_NEED_MORE;
        if (data[header_len + blen] != '\r' || data[header_len + blen + 1] != '\n') {
            return PARSE_ERR;
        }
        out.type = RESP_STR;
        out.str_val.assign((const char *)(data + header_len), (size_t)blen);
        consumed = header_len + (size_t)blen + 2;
        return PARSE_OK;
    }
    else if (tag == '*') {
        const uint8_t *crlf = resp_find_crlf(data + 1, len - 1);
        if (!crlf) return PARSE_NEED_MORE;
        std::string scnt((const char *)(data + 1), (size_t)(crlf - (data + 1)));
        long long count = 0;
        try {
            count = std::stoll(scnt);
        } catch (...) {
            return PARSE_ERR;
        }
        size_t pos = (size_t)(crlf - data) + 2;
        if (count == -1) {
            out.type = RESP_NIL;
            consumed = pos;
            return PARSE_OK;
        }
        if (count < 0 || (size_t)count > k_max_array_len) return PARSE_ERR;
        out.type = RESP_ARR;
        out.arr_val.clear();
        for (long long i = 0; i < count; i++) {
            size_t elem_consumed = 0;
            RespValue elem;
            ParseStatus st = parse_resp_value(data + pos, len - pos, elem_consumed, elem);
            if (st != PARSE_OK) return st;
            out.arr_val.push_back(elem);
            pos += elem_consumed;
        }
        consumed = pos;
        return PARSE_OK;
    }
    return PARSE_ERR;
}

// Send a RESP command across a blocking socket
#ifdef _WIN32
static inline int resp_send_cmd(SOCKET fd, const std::vector<std::string> &cmd) {
#else
static inline int resp_send_cmd(int fd, const std::vector<std::string> &cmd) {
#endif
    std::string payload;
    resp_out_arr_header(payload, cmd.size());
    for (const std::string &s : cmd) {
        resp_out_bulk(payload, s);
    }
    const char *p = payload.data();
    size_t rem = payload.size();
    while (rem > 0) {
        int rv = send(fd, p, (int)rem, 0);
        if (rv <= 0) return -1;
        p += rv;
        rem -= (size_t)rv;
    }
    return 0;
}

// Read a complete RESP value from a blocking socket using a persistent buffer (essential for pipelining)
#ifdef _WIN32
static inline int resp_read_val(SOCKET fd, std::string &buf, RespValue &out) {
#else
static inline int resp_read_val(int fd, std::string &buf, RespValue &out) {
#endif
    char chunk[4096];
    while (true) {
        if (!buf.empty()) {
            size_t consumed = 0;
            ParseStatus st = parse_resp_value((const uint8_t*)buf.data(), buf.size(), consumed, out);
            if (st == PARSE_OK) {
                buf.erase(0, consumed);
                return 0;
            }
            if (st == PARSE_ERR) {
                return -1;
            }
        }
        int rv = recv(fd, chunk, sizeof(chunk), 0);
        if (rv <= 0) {
            return -1;
        }
        buf.append(chunk, (size_t)rv);
    }
}

// Overload for one-off non-pipelined calls
#ifdef _WIN32
static inline int resp_read_val(SOCKET fd, RespValue &out) {
#else
static inline int resp_read_val(int fd, RespValue &out) {
#endif
    std::string buf;
    return resp_read_val(fd, buf, out);
}

#endif // RESP_H
