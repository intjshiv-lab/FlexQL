/*
 * thread_pool.h — fixed-size thread pool
 *
 * Nothing groundbreaking here. Condition variable + task queue.
 * Sized to match hardware threads by default.
 */

#ifndef FLEXQL_THREAD_POOL_H
#define FLEXQL_THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <stdexcept>

namespace flexql {

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads);
    ~ThreadPool();

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a task and get a future for its result
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>;

    // Get the number of worker threads
    size_t size() const { return workers_.size(); }

    // Get the number of pending tasks
    size_t pending() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    // Shutdown the pool (waits for all tasks to complete)
    void shutdown();

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;

    mutable std::mutex                queue_mutex_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
    std::atomic<bool>                 shutdown_called_{false};
};

// ---------------------------------------------------------------------------
//  Template implementation (must be in header)
// ---------------------------------------------------------------------------

template<typename F, typename... Args>
auto ThreadPool::submit(F&& f, Args&&... args)
    -> std::future<typename std::invoke_result<F, Args...>::type>
{
    using return_type = typename std::invoke_result<F, Args...>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<return_type> result = task->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (stop_) {
            throw std::runtime_error("submit() called on stopped ThreadPool");
        }
        tasks_.emplace([task]() { (*task)(); });
    }
    cv_.notify_one();

    return result;
}

}  // namespace flexql

#endif /* FLEXQL_THREAD_POOL_H */
