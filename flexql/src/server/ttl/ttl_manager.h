/*
 * ttl_manager.h — background row expiration
 *
 * Spawns a thread that periodically sweeps all registered tables
 * and removes rows past their TTL. Uses condition_variable so
 * shutdown is clean (no sleeping through a signal).
 */

#ifndef FLEXQL_TTL_MANAGER_H
#define FLEXQL_TTL_MANAGER_H

#include "server/storage/table.h"
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

namespace flexql {

class TTLManager {
public:
    // interval_sec: how often to sweep, in seconds
    explicit TTLManager(uint32_t interval_sec = 30);
    ~TTLManager();

    // Register a table for TTL sweeping
    void register_table(const std::string& name, Table* table);

    // Unregister a table
    void unregister_table(const std::string& name);

    // Start the background sweep thread
    void start();

    // Stop the background sweep thread
    void stop();

    // Force an immediate sweep (for testing)
    size_t sweep_now();

private:
    void sweep_loop();

    uint32_t interval_sec_;
    std::atomic<bool> running_{false};
    std::thread sweep_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    std::unordered_map<std::string, Table*> tables_;
    std::mutex tables_mutex_;
};

}  // namespace flexql

#endif /* FLEXQL_TTL_MANAGER_H */
