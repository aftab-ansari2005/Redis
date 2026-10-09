// serialization.h — Chapter 9: Tagged binary serialization for typed responses.
//
// DESIGN:
//   Every response value is prefixed with a 1-byte type tag that tells the
//   reader how to interpret the bytes that follow. This is a TLV-ish
//   (Type-Length-Value) scheme, minus the explicit "L" for fixed-size types.
//
//   Tag byte  | Payload
//   ----------|--------------------------------------------------------------
//   SER_NIL   | (nothing — 0 extra bytes)
//   SER_ERR   | [4-byte LE code][4-byte LE msg_len][msg bytes]
//   SER_STR   | [4-byte LE len][string bytes]
//   SER_INT   | [8-byte LE int64]
//   SER_DBL   | [8-byte IEEE 754 double, LE]
//   SER_ARR   | [4-byte LE element count][element₁][element₂]...
//             |   (each element is itself a tag + payload — recursive)
//
// WHY A BINARY FORMAT INSTEAD OF TEXT (LIKE REAL RESP)?
//
//   Tradeoff 1 — Parsing speed:
//     Binary: memcpy a fixed number of bytes → done. O(1) for integers/doubles.
//     Text:   scan for delimiter, atoi/strtod, handle edge cases. O(digits).
//     Winner: binary, especially for numeric-heavy workloads.
//
//   Tradeoff 2 — Human readability / debuggability:
//     Binary: need a pretty-printer or hex dump to inspect on the wire.
//     Text:   `nc localhost 6379` and type commands by hand.
//     Winner: text (RESP). This is why real Redis chose it.
//
//   Tradeoff 3 — Compactness:
//     Binary: an int64 is always 9 bytes (1 tag + 8 value).
//     Text:   "+42\r\n" is 5 bytes, but "+9999999999999999\r\n" is 20 bytes.
//     Winner: binary for large values, text for small values.
//
//   Tradeoff 4 — Simplicity of implementation:
//     Binary: no escaping, no delimiter conflicts, straightforward memcpy.
//     Text:   must handle inline newlines in bulk strings, count bytes anyway.
//     Winner: binary. (RESP's bulk strings already need a length prefix.)
//
//   We chose binary because:
//     (a) We already have a length-prefixed outer frame, so telnet-friendliness
//         is already lost — might as well go full binary inside.
//     (b) Simpler parser code, fewer edge cases = fewer bugs.
//     (c) It's the format the book uses, and it teaches TLV encoding well.
//

#ifndef SERIALIZATION_H
#define SERIALIZATION_H

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

// --------------------------------------------------------------------------
// Type tags — 1-byte enum identifying the payload format that follows.
//
// These values are part of the wire protocol. Once deployed, you can ADD
// new tags but must NEVER change existing ones (that would break old clients).
// --------------------------------------------------------------------------
enum SerType : uint8_t {
    SER_NIL = 0,    // null / not-found (no payload)
    SER_ERR = 1,    // error: [4-byte code][4-byte msg_len][msg]
    SER_STR = 2,    // string: [4-byte len][bytes]
    SER_INT = 3,    // 64-bit signed integer: [8 bytes LE]
    SER_DBL = 4,    // 64-bit double: [8 bytes LE / IEEE 754]
    SER_ARR = 5,    // array: [4-byte count][element...]  (recursive)
};

// --------------------------------------------------------------------------
// Serialization helpers — append typed data to an output buffer.
//
// These are "push" style: each function appends bytes to `out`. To build
// a response, call the appropriate out_* function. For arrays, call
// out_arr() with the count, then call out_* for each element.
//
// Why std::vector<uint8_t>?
//   It auto-grows, no manual buffer management. For a from-scratch project
//   that's a good trade. In production you'd use a pre-allocated arena or
//   write directly into the connection's wbuf to avoid the copy.
// --------------------------------------------------------------------------

// Append raw bytes (internal helper)
static inline void buf_append(std::vector<uint8_t> &out, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    out.insert(out.end(), p, p + len);
}

// Serialize a NIL (null) value.
// Wire: [SER_NIL]  — just the tag, zero additional bytes.
static inline void out_nil(std::vector<uint8_t> &out) {
    out.push_back(SER_NIL);
}

// Serialize an error with a numeric code and a message string.
// Wire: [SER_ERR][code:u32 LE][msg_len:u32 LE][msg bytes]
//
// The error code is application-defined (e.g. 1 = generic error, 2 = type
// error, etc.). The message is a human-readable string for diagnostics.
static inline void out_err(std::vector<uint8_t> &out, uint32_t code, const std::string &msg) {
    out.push_back(SER_ERR);
    uint32_t mlen = (uint32_t)msg.size();
    buf_append(out, &code, 4);
    buf_append(out, &mlen, 4);
    buf_append(out, msg.data(), mlen);
}

// Serialize a bulk string (arbitrary bytes, not null-terminated).
// Wire: [SER_STR][len:u32 LE][bytes]
static inline void out_str(std::vector<uint8_t> &out, const std::string &val) {
    out.push_back(SER_STR);
    uint32_t len = (uint32_t)val.size();
    buf_append(out, &len, 4);
    buf_append(out, val.data(), len);
}

// Overload: serialize from a raw C string.
static inline void out_str(std::vector<uint8_t> &out, const char *val) {
    out_str(out, std::string(val));
}

// Serialize a 64-bit signed integer.
// Wire: [SER_INT][value:i64 LE]
static inline void out_int(std::vector<uint8_t> &out, int64_t val) {
    out.push_back(SER_INT);
    buf_append(out, &val, 8);
}

// Serialize a 64-bit double.
// Wire: [SER_DBL][value:f64 LE / IEEE 754]
static inline void out_dbl(std::vector<uint8_t> &out, double val) {
    out.push_back(SER_DBL);
    buf_append(out, &val, 8);
}

// Begin serializing an array of `count` elements.
// Wire: [SER_ARR][count:u32 LE]
//
// After calling this, the caller MUST serialize exactly `count` values
// (each with its own type tag) immediately after. Arrays can nest —
// an element of an array can itself be an array.
//
// Example — serialize ["hello", 42]:
//   out_arr(out, 2);
//   out_str(out, "hello");
//   out_int(out, 42);
static inline void out_arr(std::vector<uint8_t> &out, uint32_t count) {
    out.push_back(SER_ARR);
    buf_append(out, &count, 4);
}

// --------------------------------------------------------------------------
// Deserialization helpers — read typed data from a byte buffer.
//
// These are "pull" style: given a data pointer and remaining length,
// each function reads one typed value and advances the cursor.
// Returns true on success, false on malformed data.
//
// For arrays, on_entry() reads the count and advances past the header;
// the caller then reads `count` elements in a loop (recursively).
// --------------------------------------------------------------------------

// Read a single byte from the buffer. Returns false if no bytes remain.
static inline bool read_u8(const uint8_t *&data, size_t &len, uint8_t &out) {
    if (len < 1) return false;
    out = data[0];
    data += 1;
    len -= 1;
    return true;
}

// Read a 32-bit unsigned integer (LE) from the buffer.
static inline bool read_u32(const uint8_t *&data, size_t &len, uint32_t &out) {
    if (len < 4) return false;
    memcpy(&out, data, 4);
    data += 4;
    len -= 4;
    return true;
}

// Read a 64-bit signed integer (LE) from the buffer.
static inline bool read_i64(const uint8_t *&data, size_t &len, int64_t &out) {
    if (len < 8) return false;
    memcpy(&out, data, 8);
    data += 8;
    len -= 8;
    return true;
}

// Read a 64-bit double (LE / IEEE 754) from the buffer.
static inline bool read_f64(const uint8_t *&data, size_t &len, double &out) {
    if (len < 8) return false;
    memcpy(&out, data, 8);
    data += 8;
    len -= 8;
    return true;
}

// Read a length-prefixed string from the buffer.
static inline bool read_str(const uint8_t *&data, size_t &len, std::string &out) {
    uint32_t slen;
    if (!read_u32(data, len, slen)) return false;
    if (len < slen) return false;
    out.assign((const char *)data, slen);
    data += slen;
    len -= slen;
    return true;
}

#endif // SERIALIZATION_H
