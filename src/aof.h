// aof.h — Append-Only File (AOF) persistence for Build-Your-Own Redis
#ifndef AOF_H
#define AOF_H

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include "resp.h"
#include "thread_pool.h"

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

    // AOF Rewrite state (compaction)
    bool rewrite_in_progress = false;
    volatile bool rewrite_worker_done = false;
    std::string rewrite_temp_filename;
    std::string rewrite_buf; // buffer for writes arriving while background rewrite is active

    // Transaction state
    bool tx_active = false;
    std::string tx_buf; // buffer for mutating commands during an EXEC transaction block
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

// Cross-platform atomic file rename (replaces existing destination)
static inline bool aof_atomic_rename(const char *old_path, const char *new_path) {
#ifdef _WIN32
    return MoveFileExA(old_path, new_path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(old_path, new_path) == 0;
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

    // If an EXEC transaction block is currently active, buffer this command into tx_buf!
    if (g_aof.tx_active) {
        g_aof.tx_buf.append(buf);
        return;
    }

    // If a background rewrite is in progress, buffer this mutating write!
    if (g_aof.rewrite_in_progress) {
        g_aof.rewrite_buf.append(buf);
    }

    size_t written = fwrite(buf.data(), 1, buf.size(), g_aof.fp);
    if (written != buf.size()) {
        fprintf(stderr, "[aof] Warning: partial write to AOF file\n");
    }

    if (g_aof.fsync_mode == AOF_FSYNC_ALWAYS) {
        aof_fsync_file(g_aof.fp);
    }
}

// --------------------------------------------------------------------------
// Transaction Logging Helpers (Atomic MULTI ... EXEC block in AOF)
// --------------------------------------------------------------------------
static inline void aof_tx_begin() {
    g_aof.tx_active = true;
    g_aof.tx_buf.clear();
}

static inline void aof_tx_commit() {
    if (!g_aof.tx_active) return;
    g_aof.tx_active = false;

    if (g_aof.tx_buf.empty() || !g_aof.fp) {
        return;
    }

    std::string block;
    // *1\r\n$5\r\nMULTI\r\n
    resp_out_arr_header(block, 1);
    resp_out_bulk(block, "MULTI");

    // All mutating operations executed in this transaction
    block.append(g_aof.tx_buf);

    // *1\r\n$4\r\nEXEC\r\n
    resp_out_arr_header(block, 1);
    resp_out_bulk(block, "EXEC");

    g_aof.tx_buf.clear();

    if (g_aof.rewrite_in_progress) {
        g_aof.rewrite_buf.append(block);
    }

    size_t written = fwrite(block.data(), 1, block.size(), g_aof.fp);
    if (written != block.size()) {
        fprintf(stderr, "[aof] Warning: partial write of transaction block\n");
    }

    if (g_aof.fsync_mode == AOF_FSYNC_ALWAYS) {
        aof_fsync_file(g_aof.fp);
    }
}

// --------------------------------------------------------------------------
// Background AOF Rewrite (Compaction)
// --------------------------------------------------------------------------
struct AofRewriteTask {
    std::string temp_filename;
    std::string snapshot_data;
};

static inline void aof_rewrite_worker(void *arg) {
    AofRewriteTask *task = (AofRewriteTask *)arg;

    FILE *fp = fopen(task->temp_filename.c_str(), "wb");
    if (fp) {
        if (!task->snapshot_data.empty()) {
            fwrite(task->snapshot_data.data(), 1, task->snapshot_data.size(), fp);
        }
        aof_fsync_file(fp);
        fclose(fp);
    } else {
        fprintf(stderr, "[aof] Failed to open temp rewrite file '%s'\n", task->temp_filename.c_str());
    }

    delete task;
    g_aof.rewrite_worker_done = true;
}

// Start background AOF rewrite using ThreadPool
static inline bool aof_start_bgrewrite(ThreadPool *tp, std::string &snapshot_data, const std::string &temp_filename) {
    if (g_aof.rewrite_in_progress) {
        return false;
    }

    g_aof.rewrite_in_progress = true;
    g_aof.rewrite_worker_done = false;
    g_aof.rewrite_temp_filename = temp_filename;
    g_aof.rewrite_buf.clear();

    AofRewriteTask *task = new AofRewriteTask();
    task->temp_filename = temp_filename;
    task->snapshot_data = std::move(snapshot_data);

    thread_pool_queue(tp, &aof_rewrite_worker, task);
    return true;
}

// Synchronous AOF rewrite
static inline bool aof_rewrite_sync(const std::string &snapshot_data, const std::string &temp_filename) {
    if (g_aof.rewrite_in_progress) {
        return false;
    }

    FILE *fp = fopen(temp_filename.c_str(), "wb");
    if (!fp) return false;

    if (!snapshot_data.empty()) {
        fwrite(snapshot_data.data(), 1, snapshot_data.size(), fp);
    }
    aof_fsync_file(fp);
    fclose(fp);

    aof_close();
    if (!aof_atomic_rename(temp_filename.c_str(), g_aof.filename.c_str())) {
        fprintf(stderr, "[aof] Error: rename failed during synchronous rewrite\n");
    }
    aof_init(g_aof.filename, g_aof.fsync_mode);
    return true;
}

// Called from event loop to check if the worker finished writing the snapshot
static inline bool aof_check_rewrite_done() {
    if (!g_aof.rewrite_in_progress || !g_aof.rewrite_worker_done) {
        return false;
    }

    // 1. Append commands that arrived in the rewrite buffer while the worker was writing
    if (!g_aof.rewrite_buf.empty()) {
        FILE *fp = fopen(g_aof.rewrite_temp_filename.c_str(), "ab");
        if (fp) {
            fwrite(g_aof.rewrite_buf.data(), 1, g_aof.rewrite_buf.size(), fp);
            aof_fsync_file(fp);
            fclose(fp);
        }
        g_aof.rewrite_buf.clear();
    }

    // 2. Close active AOF handle
    aof_close();

    // 3. Atomically replace old AOF with newly compacted file
    if (!aof_atomic_rename(g_aof.rewrite_temp_filename.c_str(), g_aof.filename.c_str())) {
        fprintf(stderr, "[aof] Error: atomic rename '%s' -> '%s' failed!\n",
                g_aof.rewrite_temp_filename.c_str(), g_aof.filename.c_str());
    }

    // 4. Reopen compacted AOF in append mode
    aof_init(g_aof.filename, g_aof.fsync_mode);

    g_aof.rewrite_in_progress = false;
    g_aof.rewrite_worker_done = false;

    printf("[aof] Background AOF rewrite completed successfully.\n");
    return true;
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

    bool in_aof_multi = false;
    size_t multi_start_offset = 0;
    std::vector<std::vector<std::string>> aof_multi_cmds;

    while (offset < bytes_read) {
        size_t consumed = 0;
        std::vector<std::string> cmd;
        ParseStatus st = parse_request(buf.data() + offset, bytes_read - offset, consumed, cmd);

        if (st == PARSE_OK) {
            std::string c = cmd.empty() ? "" : cmd[0];
            for (char &ch : c) ch = (char)toupper((unsigned char)ch);

            if (c == "MULTI") {
                in_aof_multi = true;
                multi_start_offset = offset;
                aof_multi_cmds.clear();
            } else if (c == "EXEC") {
                if (in_aof_multi) {
                    for (const auto &tcmd : aof_multi_cmds) {
                        dispatch_fn(tcmd);
                        replayed_count++;
                    }
                    in_aof_multi = false;
                    aof_multi_cmds.clear();
                    offset += consumed;
                    valid_bytes = offset;
                    continue;
                }
            } else if (in_aof_multi) {
                aof_multi_cmds.push_back(cmd);
            } else {
                dispatch_fn(cmd);
                replayed_count++;
            }

            offset += consumed;
            if (!in_aof_multi) {
                valid_bytes = offset;
            }
        } else if (st == PARSE_NEED_MORE || st == PARSE_ERR) {
            truncated_tail = true;
            break;
        }
    }

    // If EOF was reached while an open MULTI block was not terminated with EXEC,
    // the transaction was never committed! Roll back and truncate to multi_start_offset.
    if (in_aof_multi) {
        truncated_tail = true;
        valid_bytes = multi_start_offset;
        fprintf(stderr, "[aof] Warning: incomplete MULTI transaction block at end of '%s'. Rolling back and truncating.\n",
                filename.c_str());
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
