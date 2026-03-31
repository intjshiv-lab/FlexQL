/*
 * thread_pool.cpp
 */

#include "server/concurrency/thread_pool.h"

namespace flexql {

ThreadPool::ThreadPool(size_t num_threads) {
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back([this]() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    cv_.wait(lock, [this]() {
                        return stop_.load() || !tasks_.empty();
                    });
                    if (stop_.load() && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    bool expected = false;
    if (!shutdown_called_.compare_exchange_strong(expected, true)) return;

    stop_.store(true);
    cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

}  // namespace flexql
