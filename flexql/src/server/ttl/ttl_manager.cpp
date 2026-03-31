/*
 * ttl_manager.cpp
 */

#include "server/ttl/ttl_manager.h"
#include <chrono>

namespace flexql {

TTLManager::TTLManager(uint32_t interval_sec) : interval_sec_(interval_sec) {}

TTLManager::~TTLManager() {
    stop();
}

void TTLManager::register_table(const std::string& name, Table* table) {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    tables_[name] = table;
}

void TTLManager::unregister_table(const std::string& name) {
    std::lock_guard<std::mutex> lock(tables_mutex_);
    tables_.erase(name);
}

void TTLManager::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    sweep_thread_ = std::thread(&TTLManager::sweep_loop, this);
}

void TTLManager::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    cv_.notify_all();
    if (sweep_thread_.joinable()) {
        sweep_thread_.join();
    }
}

size_t TTLManager::sweep_now() {
    size_t total_removed = 0;
    std::lock_guard<std::mutex> lock(tables_mutex_);
    for (auto& [name, table] : tables_) {
        total_removed += table->cleanup_expired();
    }
    return total_removed;
}

void TTLManager::sweep_loop() {
    while (running_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(interval_sec_), [this]() {
                return !running_.load();
            });
        }
        if (!running_.load()) break;
        sweep_now();
    }
}

}  // namespace flexql
