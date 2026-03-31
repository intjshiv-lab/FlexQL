/*
 * database.h — top-level database object
 *
 * Owns the executor, TTL manager, lock manager, WAL, and snapshot.
 * When data_dir is set, all mutations are logged to WAL before ack.
 * On startup, snapshot is loaded and WAL is replayed.
 */

#ifndef FLEXQL_DATABASE_H
#define FLEXQL_DATABASE_H

#include "server/executor/executor.h"
#include "server/concurrency/lock_manager.h"
#include "server/ttl/ttl_manager.h"
#include "server/storage/wal.h"
#include "server/storage/snapshot.h"
#include <string>
#include <memory>

namespace flexql {

class Database {
public:
    // data_dir: empty string = in-memory only (original behavior)
    //           non-empty   = persistent mode (WAL + snapshot)
    explicit Database(const std::string& data_dir = "",
                      uint32_t ttl_sweep_interval = 30);
    ~Database();

    // Execute a raw SQL string
    Executor::QueryResult execute(const std::string& sql);

    // Access components
    Executor&    executor()     { return executor_; }
    LockManager& lock_manager() { return lock_mgr_; }
    TTLManager&  ttl_manager()  { return ttl_mgr_; }

    // Start/stop background services (TTL + snapshot)
    void start();
    void stop();

    // Force a snapshot now (can be called manually or by timer)
    bool take_snapshot(std::string& error);

    // Is persistence enabled?
    bool persistent() const { return !data_dir_.empty(); }
    const std::string& data_dir() const { return data_dir_; }

    // Recovery stats
    uint64_t recovered_wal_records() const { return recovered_wal_records_; }

private:
    // Recovery: load snapshot + replay WAL
    void recover();

    std::string data_dir_;
    Executor    executor_;
    LockManager lock_mgr_;
    TTLManager  ttl_mgr_;

    // Persistence (only active when data_dir_ is non-empty)
    std::unique_ptr<WALWriter> wal_writer_;
    uint64_t snapshot_lsn_          = 0;
    uint64_t recovered_wal_records_ = 0;

    bool started_ = false;
};

}  // namespace flexql

#endif /* FLEXQL_DATABASE_H */
