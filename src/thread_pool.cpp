#include "thread_pool.h"
#include <cassert>

#ifdef _WIN32
#include <process.h>

// Worker thread routine for Win32
static unsigned __stdcall worker_win32(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;
    while (true) {
        EnterCriticalSection(&tp->mu);
        // Wait until there is work or the pool is stopped
        while (tp->queue.empty() && !tp->stop) {
            SleepConditionVariableCS(&tp->not_empty, &tp->mu, INFINITE);
        }
        if (tp->stop && tp->queue.empty()) {
            LeaveCriticalSection(&tp->mu);
            break;
        }

        Work w = tp->queue.front();
        tp->queue.pop_front();
        LeaveCriticalSection(&tp->mu);

        // Execute task outside the lock
        if (w.f) {
            w.f(w.arg);
        }
    }
    return 0;
}

void thread_pool_init(ThreadPool *tp, size_t num_threads) {
    assert(num_threads > 0);
    tp->stop = false;
    InitializeCriticalSection(&tp->mu);
    InitializeConditionVariable(&tp->not_empty);

    tp->threads.resize(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        tp->threads[i] = (HANDLE)_beginthreadex(NULL, 0, &worker_win32, tp, 0, NULL);
        assert(tp->threads[i] != NULL);
    }
}

void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg) {
    Work w;
    w.f = f;
    w.arg = arg;

    EnterCriticalSection(&tp->mu);
    tp->queue.push_back(w);
    WakeConditionVariable(&tp->not_empty);
    LeaveCriticalSection(&tp->mu);
}

void thread_pool_destroy(ThreadPool *tp) {
    EnterCriticalSection(&tp->mu);
    tp->stop = true;
    WakeAllConditionVariable(&tp->not_empty);
    LeaveCriticalSection(&tp->mu);

    for (size_t i = 0; i < tp->threads.size(); ++i) {
        WaitForSingleObject(tp->threads[i], INFINITE);
        CloseHandle(tp->threads[i]);
    }
    tp->threads.clear();
    DeleteCriticalSection(&tp->mu);
}

#else

// Worker thread routine for POSIX
static void *worker_posix(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;
    while (true) {
        pthread_mutex_lock(&tp->mu);
        while (tp->queue.empty() && !tp->stop) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }
        if (tp->stop && tp->queue.empty()) {
            pthread_mutex_unlock(&tp->mu);
            break;
        }

        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);

        if (w.f) {
            w.f(w.arg);
        }
    }
    return NULL;
}

void thread_pool_init(ThreadPool *tp, size_t num_threads) {
    assert(num_threads > 0);
    tp->stop = false;
    int rv = pthread_mutex_init(&tp->mu, NULL);
    assert(rv == 0);
    rv = pthread_cond_init(&tp->not_empty, NULL);
    assert(rv == 0);

    tp->threads.resize(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        rv = pthread_create(&tp->threads[i], NULL, &worker_posix, tp);
        assert(rv == 0);
    }
}

void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg) {
    Work w;
    w.f = f;
    w.arg = arg;

    pthread_mutex_lock(&tp->mu);
    tp->queue.push_back(w);
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
}

void thread_pool_destroy(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mu);
    tp->stop = true;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);

    for (size_t i = 0; i < tp->threads.size(); ++i) {
        pthread_join(tp->threads[i], NULL);
    }
    tp->threads.clear();
    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
}

#endif
