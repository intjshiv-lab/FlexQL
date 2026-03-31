/*
 * lock_manager.cpp — per-table read/write locks
 *
 * Lazily creates a shared_mutex for each table on first access.
 * Could use a striped lock array instead but this is simple and
 * we don't have that many tables anyway.
 */

#include "server/concurrency/lock_manager.h"

namespace flexql {

std::shared_mutex& LockManager::get_lock(const std::string& table_name) {
    std::lock_guard<std::mutex> guard(map_mutex_);
    auto it = locks_.find(table_name);
    if (it == locks_.end()) {
        auto [inserted, _] = locks_.emplace(table_name,
                                             std::make_unique<std::shared_mutex>());
        return *inserted->second;
    }
    return *it->second;
}

}  // namespace flexql
