#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include "MutatorSync.h"

namespace {
std::mutex g_mutex;
std::condition_variable g_cv;
std::vector<MutatorThread *> g_threads;
std::atomic<bool> g_stwRequested{false};
} // namespace

extern "C" MutatorThread *MutatorSync_RegisterThread(word_t **stackBottom) {
    auto *thread = (MutatorThread *)malloc(sizeof(MutatorThread));
    thread->allocator = NULL; // set by the caller right after this returns
    thread->stackBottom = stackBottom;
    thread->parkedStackTop = NULL;
    thread->parked = false;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_threads.push_back(thread);
    return thread;
}

extern "C" void MutatorSync_UnregisterThread(MutatorThread *self) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), self),
                    g_threads.end());
    free(self);
}

extern "C" uint32_t MutatorSync_ThreadCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (uint32_t)g_threads.size();
}

extern "C" void MutatorSync_Poll(MutatorThread *self) {
    // Fast, unlocked path: no collection pending, nothing to do. This is
    // the only cost MutatorSync adds to a thread that never collides with
    // a collection.
    if (!g_stwRequested.load(std::memory_order_acquire)) {
        return;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    // Re-check under the lock: the request may have already been resolved
    // between the unlocked check above and acquiring the lock here.
    if (!g_stwRequested.load(std::memory_order_acquire)) {
        return;
    }

    self->parkedStackTop = (word_t *)&self;
    self->parked = true;
    g_cv.notify_all(); // tell a waiting collector "one more parked"
    g_cv.wait(lock, [] { return !g_stwRequested.load(std::memory_order_acquire); });
    self->parked = false;
}

extern "C" bool MutatorSync_BeginCollection(MutatorThread *self) {
    bool expected = false;
    if (!g_stwRequested.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
        // Someone else is already collecting (or just won the race to
        // start). Park like a normal mutator and let them drive it.
        MutatorSync_Poll(self);
        return false;
    }

    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [&] {
        for (MutatorThread *t : g_threads) {
            if (t != self && !t->parked) {
                return false;
            }
        }
        return true;
    });
    return true;
}

extern "C" void MutatorSync_EndCollection() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stwRequested.store(false, std::memory_order_release);
    g_cv.notify_all();
}

extern "C" void MutatorSync_ForEachThread(MutatorSync_ThreadVisitor visitor,
                                          void *userData) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (MutatorThread *t : g_threads) {
        visitor(t, userData);
    }
}