// test_lazyfree.cpp — Empirical before/after latency comparison test
//
// Chapter 14: Thread Pool & Lazy Free
//
// WHAT THIS TESTS:
//   Demonstrates empirically how a single-threaded event loop without a thread pool
//   suffers from severe latency spikes when destroying large collections, and how
//   offloading destruction to a background thread pool eliminates those spikes.
//
// ARCHITECTURE OF THE TEST:
//   Two concurrent TCP clients talk to server.exe:
//     - Client A (Mutator): Sets config, populates a huge sorted set, and calls DEL.
//     - Client B (Probe): Continually sends PING requests on a separate socket,
//       measuring round-trip latency with microsecond precision.
//
//   Run 1: Synchronous DEL (CONFIG SET lazyfree 0)
//          Event loop blocks while freeing all nodes -> Client B latency SPIKES.
//   Run 2: Asynchronous DEL with Thread Pool (CONFIG SET lazyfree 1)
//          Event loop detaches in O(1) time and hands off to worker -> Client B latency unaffected.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <numeric>
#include "../src/resp.h"

static void die(const char *msg) {
    fprintf(stderr, "[FAIL] %s (WSA error: %d)\n", msg, WSAGetLastError());
    WSACleanup();
    exit(1);
}

static inline int send_request(SOCKET fd, const std::vector<std::string> &cmd) {
    return resp_send_cmd(fd, cmd);
}

static inline int read_response_raw(SOCKET fd, RespValue &out) {
    return resp_read_val(fd, out);
}

static SOCKET connect_client() {
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) die("socket()");

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        die("connect() to 127.0.0.1:1234 — Is server.exe running?");
    }
    return fd;
}

// --------------------------------------------------------------------------
// Background Latency Probe Client
// --------------------------------------------------------------------------
struct ProbeThreadData {
    SOCKET fd;
    volatile bool stop;
    std::vector<double> latencies_ms;
    double max_latency_ms;
};

static unsigned __stdcall probe_worker(void *arg) {
    ProbeThreadData *data = (ProbeThreadData *)arg;
    std::vector<std::string> ping_cmd = {"ping"};
    RespValue resp;

    data->max_latency_ms = 0.0;
    data->latencies_ms.reserve(50000);

    while (!data->stop) {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (send_request(data->fd, ping_cmd) != 0) break;
        if (read_response_raw(data->fd, resp) != 0) break;
        auto t1 = std::chrono::high_resolution_clock::now();

        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        data->latencies_ms.push_back(elapsed_ms);
        if (elapsed_ms > data->max_latency_ms) {
            data->max_latency_ms = elapsed_ms;
        }

        // Sleep 0 to yield CPU, preventing saturation of client core
        Sleep(0);
    }
    return 0;
}

// --------------------------------------------------------------------------
// Measurement Runner
// --------------------------------------------------------------------------
struct BenchmarkResult {
    bool lazyfree;
    int num_items;
    double del_duration_ms;
    double probe_max_latency_ms;
    double probe_avg_latency_ms;
    double probe_p99_latency_ms;
    size_t probe_samples;
};

static BenchmarkResult run_test_pass(SOCKET client_a, SOCKET client_b, bool enable_lazyfree, int num_items) {
    BenchmarkResult res;
    res.lazyfree = enable_lazyfree;
    res.num_items = num_items;

    // 1. Set lazyfree mode on server
    RespValue resp;
    send_request(client_a, {"config", "set", "lazyfree", enable_lazyfree ? "1" : "0"});
    read_response_raw(client_a, resp);

    // 2. Populate huge ZSet
    printf("  Populating huge sorted set with %d members... ", num_items);
    fflush(stdout);
    auto t_pop0 = std::chrono::high_resolution_clock::now();
    send_request(client_a, {"zpopulate", "huge_zset", std::to_string(num_items)});
    read_response_raw(client_a, resp);
    auto t_pop1 = std::chrono::high_resolution_clock::now();
    double pop_time_ms = std::chrono::duration<double, std::milli>(t_pop1 - t_pop0).count();
    printf("done in %.1f ms.\n", pop_time_ms);

    // 3. Start background probe thread on client B
    ProbeThreadData probe_data;
    probe_data.fd = client_b;
    probe_data.stop = false;
    HANDLE probe_thread = (HANDLE)_beginthreadex(NULL, 0, &probe_worker, &probe_data, 0, NULL);

    // Allow probe thread to warm up and record a few baseline samples
    Sleep(50);

    // 4. Client A issues DEL and measures duration
    printf("  Client A issuing DEL huge_zset (lazyfree=%s)... ", enable_lazyfree ? "ENABLED" : "DISABLED");
    fflush(stdout);
    auto t_del0 = std::chrono::high_resolution_clock::now();
    send_request(client_a, {"del", "huge_zset"});
    read_response_raw(client_a, resp);
    auto t_del1 = std::chrono::high_resolution_clock::now();
    res.del_duration_ms = std::chrono::duration<double, std::milli>(t_del1 - t_del0).count();
    printf("completed in %.3f ms.\n", res.del_duration_ms);

    // Give background workers time to finish freeing
    Sleep(100);

    // 5. Stop probe thread
    probe_data.stop = true;
    WaitForSingleObject(probe_thread, INFINITE);
    CloseHandle(probe_thread);

    // 6. Aggregate probe latency statistics
    res.probe_samples = probe_data.latencies_ms.size();
    res.probe_max_latency_ms = probe_data.max_latency_ms;

    if (!probe_data.latencies_ms.empty()) {
        double sum = std::accumulate(probe_data.latencies_ms.begin(), probe_data.latencies_ms.end(), 0.0);
        res.probe_avg_latency_ms = sum / probe_data.latencies_ms.size();
        std::vector<double> sorted_lats = probe_data.latencies_ms;
        std::sort(sorted_lats.begin(), sorted_lats.end());
        size_t p99_idx = (size_t)(sorted_lats.size() * 0.99);
        if (p99_idx >= sorted_lats.size()) p99_idx = sorted_lats.size() - 1;
        res.probe_p99_latency_ms = sorted_lats[p99_idx];
    } else {
        res.probe_avg_latency_ms = 0.0;
        res.probe_p99_latency_ms = 0.0;
    }

    return res;
}

int main(int argc, char *argv[]) {
    int num_items = 250000;
    if (argc > 1) {
        num_items = atoi(argv[1]);
        if (num_items <= 0) num_items = 250000;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        die("WSAStartup");
    }

    printf("=======================================================================\n");
    printf(" Chapter 14: Thread Pool / Lazy-Free Benchmark (%d-member ZSet)\n", num_items);
    printf("=======================================================================\n\n");

    SOCKET client_a = connect_client();
    SOCKET client_b = connect_client();

    // Warm up connections with a simple PING
    RespValue resp;
    send_request(client_a, {"ping"});
    read_response_raw(client_a, resp);
    send_request(client_b, {"ping"});
    read_response_raw(client_b, resp);

    printf("--- [Phase 1] BEFORE: Synchronous Deletion (lazyfree = 0) ---\n");
    BenchmarkResult before = run_test_pass(client_a, client_b, false, num_items);
    printf("  Client B max probe latency: %.3f ms (probes: %lu)\n\n",
           before.probe_max_latency_ms, (unsigned long)before.probe_samples);

    // Give system time to settle
    Sleep(500);

    printf("--- [Phase 2] AFTER: Asynchronous Thread Pool Deletion (lazyfree = 1) ---\n");
    BenchmarkResult after = run_test_pass(client_a, client_b, true, num_items);
    printf("  Client B max probe latency: %.3f ms (probes: %lu)\n\n",
           after.probe_max_latency_ms, (unsigned long)after.probe_samples);

    // Summary Table
    printf("=======================================================================\n");
    printf(" EMPIRICAL RESULTS & LATENCY COMPARISON TABLE\n");
    printf("=======================================================================\n");
    printf("%-26s | %-16s | %-16s\n", "Metric", "Synchronous (Old)", "Thread Pool (New)");
    printf("---------------------------+------------------+-------------------\n");
    printf("%-26s | %13.3f ms | %13.3f ms\n", "DEL Execution Time", before.del_duration_ms, after.del_duration_ms);
    printf("%-26s | %13.3f ms | %13.3f ms\n", "Max Client Latency Spike", before.probe_max_latency_ms, after.probe_max_latency_ms);
    printf("%-26s | %13.3f ms | %13.3f ms\n", "P99 Client Latency", before.probe_p99_latency_ms, after.probe_p99_latency_ms);
    printf("%-26s | %13.3f ms | %13.3f ms\n", "Avg Client Latency", before.probe_avg_latency_ms, after.probe_avg_latency_ms);
    printf("%-26s | %16lu | %16lu\n", "Probes Received", (unsigned long)before.probe_samples, (unsigned long)after.probe_samples);
    printf("---------------------------+------------------+-------------------\n");

    if (after.probe_max_latency_ms > 0) {
        double ratio = before.probe_max_latency_ms / after.probe_max_latency_ms;
        printf(">>> LATENCY SPIKE REDUCTION: %.1fx improvement!\n", ratio);
    }
    printf("=======================================================================\n");

    closesocket(client_a);
    closesocket(client_b);
    WSACleanup();
    return 0;
}
