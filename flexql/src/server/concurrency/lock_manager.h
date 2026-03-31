/*
 * lock_manager.h — per-table reader-writer locks
 */

#ifndef FLEXQL_LOCK_MANAGER_H
#define FLEXQL_LOCK_MANAGER_H

#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>

namespace flexql {

class LockManager {
public:
    LockManager() = default;
    ~LockManager() = default;

    // Get the read-write lock for a specific table (creates if absent)
    std::shared_mutex& get_lock(const std::string& table_name);

    // RAII read lock guard
    class ReadLock {
    public:
        ReadLock(LockManager& mgr, const std::string& table)
            : lock_(mgr.get_lock(table)) {}
    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    // RAII write lock guard
    class WriteLock {
    public:
        WriteLock(LockManager& mgr, const std::string& table)
            : lock_(mgr.get_lock(table)) {}
    private:
        std::unique_lock<std::shared_mutex> lock_;
    };

private:
    std::mutex                                                     map_mutex_;
    std::unordered_map<std::string, std::unique_ptr<std::shared_mutex>> locks_;
};

}  // namespace flexql

#endif /* FLEXQL_LOCK_MANAGER_H */
