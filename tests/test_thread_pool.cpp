#include "../src/thread_pool.h"
#include <cstdio>
#include <cassert>
#include <vector>

#ifdef _WIN32
#include <windows.h>
static volatile LONG g_counter = 0;
static void increment_task(void *arg) {
    (void)arg;
    InterlockedIncrement(&g_counter);
}
#else
#include <atomic>
static std::atomic<int> g_counter{0};
static void increment_task(void *arg) {
    (void)arg;
    g_counter++;
}
#endif

int main() {
    printf("Testing ThreadPool with 4 worker threads and 1000 tasks... ");
    ThreadPool tp;
    thread_pool_init(&tp, 4);

    const int N = 1000;
    for (int i = 0; i < N; i++) {
        thread_pool_queue(&tp, &increment_task, NULL);
    }

    // Destroy thread pool: waits for all tasks to complete and workers to exit
    thread_pool_destroy(&tp);

#ifdef _WIN32
    assert(g_counter == N);
#else
    assert(g_counter.load() == N);
#endif

    printf("PASSED (completed %d tasks across workers)\n", N);
    return 0;
}
