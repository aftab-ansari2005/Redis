// aof.h — Append-Only File (AOF) persistence for Build-Your-Own Redis
#ifndef AOF_H
#define AOF_H

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include "resp.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
extern "C" int _fileno(FILE *);
#else
#include <unistd.h>
#include <fcntl.h>
#endif

// --------------------------------------------------------------------------
// Fsync policies
// --------------------------------------------------------------------------
enum AofFsyncMode {
    AOF_FSYNC_ALWAYS = 0,   // fsync after every write (highest durability, lowest latency)
    AOF_FSYNC_EVERYSEC = 1, // fsync once per second in background thread
    AOF_FSYNC_NO = 2,       // leave fsync to OS kernel
};

// Global AOF state
struct AofState {
    FILE *fp = NULL;
    std::string filename;
    AofFsyncMode fsync_mode = AOF_FSYNC_ALWAYS;
    bool loading = false;
};

// Defined in server.cpp (or test file)
extern AofState g_aof;

// Cross-platform physical disk flush
static inline void aof_fsync_file(FILE *fp) {
    if (!fp) return;
    fflush(fp);
#ifdef _WIN32
    int fd = _fileno(fp);
    if (fd >= 0) {
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        if (h != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(h);
        }
    }
#else
    fsync(fileno(fp));
#endif
}

// Cross-platform file truncate helper (for crash recovery on truncated commands)
static inline bool aof_truncate_file(const char *filename, size_t valid_bytes) {
#ifdef _WIN32
    HANDLE h = CreateFileA(filename, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)valid_bytes;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) {
        CloseHandle(h);
        return false;
    }
    BOOL ok = SetEndOfFile(h);
    CloseHandle(h);
    return ok != FALSE;
#else
    int fd = open(filename, O_WRONLY);
    if (fd < 0) return false;
    int res = ftruncate(fd, (off_t)valid_bytes);
    close(fd);
    return res == 0;
#endif
}

// Initialize AOF subsystem and open the file in append mode
static inline bool aof_init(const std::string &filename, AofFsyncMode mode = AOF_FSYNC_ALWAYS) {
    g_aof.filename = filename;
    g_aof.fsync_mode = mode;
    g_aof.loading = false;
    g_aof.fp = fopen(filename.c_str(), "a+b");
    if (!g_aof.fp) {
        fprintf(stderr, "[aof] Failed to open AOF file '%s'\n", filename.c_str());
        return false;
    }
    return true;
}

// Close AOF file gracefully
static inline void aof_close() {
    if (g_aof.fp) {
        aof_fsync_file(g_aof.fp);
        fclose(g_aof.fp);
        g_aof.fp = NULL;
    }
}

// Append a mutating command formatted as a standard RESP array
static inline void aof_append(const std::vector<std::string> &cmd) {
    if (g_aof.loading || !g_aof.fp || cmd.empty()) {
        return;
    }

    std::string buf;
    resp_out_arr_header(buf, cmd.size());
    for (const auto &arg : cmd) {
        resp_out_bulk(buf, arg);
    }

    size_t written = fwrite(buf.data(), 1, buf.size(), g_aof.fp);
    if (written != buf.size()) {
        fprintf(stderr, "[aof] Warning: partial write to AOF file\n");
    }

    if (g_aof.fsync_mode == AOF_FSYNC_ALWAYS) {
        aof_fsync_file(g_aof.fp);
    }
}

// Replay AOF on startup:
// Reads the file, parses each RESP command using parse_request(), dispatches it,
// detects incomplete/truncated trailing command and truncates the file back to last valid byte.
template<typename Dispatcher>
static inline bool aof_load(const std::string &filename, Dispatcher dispatch_fn) {
    FILE *fp = fopen(filename.c_str(), "rb");
    if (!fp) {
        // No AOF file exists yet — clean startup
        return true;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(fp);
        return true;
    }

    std::vector<uint8_t> buf((size_t)fsize);
    size_t bytes_read = fread(buf.data(), 1, (size_t)fsize, fp);
    fclose(fp);

    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "[aof] Warning: read mismatch on '%s'\n", filename.c_str());
    }

    g_aof.loading = true;

    size_t offset = 0;
    size_t valid_bytes = 0;
    size_t replayed_count = 0;
    bool truncated_tail = false;

    while (offset < bytes_read) {
        size_t consumed = 0;
        std::vector<std::string> cmd;
        ParseStatus st = parse_request(buf.data() + offset, bytes_read - offset, consumed, cmd);

        if (st == PARSE_OK) {
            dispatch_fn(cmd);
            offset += consumed;
            valid_bytes = offset;
            replayed_count++;
        } else if (st == PARSE_NEED_MORE || st == PARSE_ERR) {
            truncated_tail = true;
            break;
        }
    }

    g_aof.loading = false;

    if (truncated_tail && valid_bytes < bytes_read) {
        fprintf(stderr, "[aof] Warning: truncated command at end of '%s'. Truncating from %u to %u bytes.\n",
                filename.c_str(), (unsigned)bytes_read, (unsigned)valid_bytes);
        aof_truncate_file(filename.c_str(), valid_bytes);
    }

    printf("[aof] Replayed %u commands from '%s' (%u bytes valid).\n",
           (unsigned)replayed_count, filename.c_str(), (unsigned)valid_bytes);
    return true;
}

#endif // AOF_H
