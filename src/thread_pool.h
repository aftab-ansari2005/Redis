#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <cstddef>
#include <vector>
#include <deque>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

// --------------------------------------------------------------------------
// Work — a single task item submitted to the thread pool.
// Consists of a function pointer and an arbitrary pointer payload.
// --------------------------------------------------------------------------
struct Work {
    void (*f)(void *) = NULL;
    void *arg = NULL;
};

// --------------------------------------------------------------------------
// ThreadPool — fixed-size worker thread pool with mutex + condvar queue.
// --------------------------------------------------------------------------
struct ThreadPool {
    std::deque<Work> queue;
    bool stop = false;

#ifdef _WIN32
    std::vector<HANDLE> threads;
    CRITICAL_SECTION mu;
    CONDITION_VARIABLE not_empty;
#else
    std::vector<pthread_t> threads;
    pthread_mutex_t mu;
    pthread_cond_t not_empty;
#endif
};

// Initialize thread pool with num_threads workers
void thread_pool_init(ThreadPool *tp, size_t num_threads);

// Enqueue a task to be executed by one of the background workers
void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg);

// Shut down the pool and wait for all workers to finish
void thread_pool_destroy(ThreadPool *tp);

#endif // THREAD_POOL_H
