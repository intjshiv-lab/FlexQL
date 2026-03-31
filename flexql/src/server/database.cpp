/*
 * database.cpp — Database with optional WAL + Snapshot persistence
 *
 * When data_dir is set:
 *   - On startup: load snapshot, then replay WAL
 *   - On every mutating query: append to WAL before acknowledging
 *   - take_snapshot() can be called to checkpoint + truncate WAL
 *
 * When data_dir is empty: original in-memory behavior, no disk I/O.
 */

#include "server/database.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace flexql {

// ─── Helper: classify SQL as mutating (CREATE/INSERT) or not ──────────────

static bool is_mutating_sql(const std::string& sql) {
    // Find first non-space character and check keyword
    size_t i = 0;
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) i++;
    if (i >= sql.size()) return false;

    // Extract first word (uppercase)
    std::string word;
    while (i < sql.size() && std::isalpha(static_cast<unsigned char>(sql[i]))) {
        word += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[i])));
        i++;
    }
    return (word == "CREATE" || word == "INSERT");
}

// ─── Constructor ──────────────────────────────────────────────────────────

Database::Database(const std::string& data_dir, uint32_t ttl_sweep_interval)
    : data_dir_(data_dir)
    , ttl_mgr_(ttl_sweep_interval)
{
    if (persistent()) {
        // Ensure data directory exists
        fs::create_directories(data_dir_);

        // Recovery: load snapshot + replay WAL
        recover();

        // Open WAL writer for new records
        std::string wal_path = data_dir_ + "/wal.log";
        wal_writer_ = std::make_unique<WALWriter>(wal_path);

        if (wal_writer_->is_open()) {
            std::cout << "[FlexQL] WAL opened: " << wal_path << "\n";
        } else {
            std::cerr << "[FlexQL] WARNING: Could not open WAL file\n";
        }
    }
}

Database::~Database() {
    stop();
    // Final snapshot on clean shutdown
    if (persistent()) {
        std::string err;
        take_snapshot(err);
    }
}

// ─── Execute ──────────────────────────────────────────────────────────────

Executor::QueryResult Database::execute(const std::string& sql) {
    // If persistent and mutating, write to WAL BEFORE executing
    if (persistent() && wal_writer_ && is_mutating_sql(sql)) {
        wal_writer_->append(sql);
    }

    return executor_.execute_sql(sql);
}

// ─── Start/Stop ───────────────────────────────────────────────────────────

void Database::start() {
    if (started_) return;
    ttl_mgr_.start();
    started_ = true;

    if (persistent()) {
        std::cout << "[FlexQL] Persistence enabled: " << data_dir_ << "\n";
        std::cout << "[FlexQL] Recovered WAL records: " << recovered_wal_records_ << "\n";
    } else {
        std::cout << "[FlexQL] Running in-memory mode (no persistence)\n";
    }
}

void Database::stop() {
    if (!started_) return;
    ttl_mgr_.stop();
    started_ = false;
}

// ─── Snapshot ─────────────────────────────────────────────────────────────

bool Database::take_snapshot(std::string& error) {
    if (!persistent()) {
        error = "Persistence not enabled";
        return false;
    }

    std::string snap_path = data_dir_ + "/snapshot.dat";
    uint64_t lsn = wal_writer_ ? wal_writer_->current_lsn() - 1 : 0;

    // Get tables from executor (need friend or public accessor)
    bool ok = Snapshot::save(snap_path, executor_.tables(), lsn, error);
    if (ok) {
        snapshot_lsn_ = lsn;
        // Truncate WAL since snapshot now covers everything
        if (wal_writer_) {
            wal_writer_->truncate();
        }
        std::cout << "[FlexQL] Snapshot saved at LSN " << lsn << "\n";
    }
    return ok;
}

// ─── Recovery ─────────────────────────────────────────────────────────────

void Database::recover() {
    std::string snap_path = data_dir_ + "/snapshot.dat";
    std::string wal_path  = data_dir_ + "/wal.log";

    // Step 1: Load snapshot if it exists
    if (fs::exists(snap_path)) {
        std::cout << "[FlexQL] Loading snapshot: " << snap_path << "\n";
        auto result = Snapshot::load(snap_path);
        if (result.success) {
            // Move tables into executor
            for (auto& [name, table] : result.tables) {
                executor_.restore_table(name, std::move(table));
            }
            snapshot_lsn_ = result.last_wal_lsn;
            std::cout << "[FlexQL] Snapshot loaded: "
                      << executor_.table_count() << " tables, LSN " << snapshot_lsn_ << "\n";
        } else {
            std::cerr << "[FlexQL] WARNING: Snapshot load failed: " << result.error << "\n";
        }
    }

    // Step 2: Replay WAL from after snapshot LSN
    if (fs::exists(wal_path)) {
        std::cout << "[FlexQL] Replaying WAL: " << wal_path << "\n";
        WALReader reader(wal_path);
        if (reader.is_open()) {
            recovered_wal_records_ = reader.replay([&](const WALRecord& rec) -> bool {
                // Skip records already covered by the snapshot
                if (rec.lsn <= snapshot_lsn_) return true;

                // Re-execute the SQL (silently)
                auto result = executor_.execute_sql(rec.sql);
                if (!result.success) {
                    std::cerr << "[WAL REPLAY] LSN " << rec.lsn
                              << " failed: " << result.error << "\n";
                }
                return true;
            });
            std::cout << "[FlexQL] WAL replay complete: "
                      << recovered_wal_records_ << " records\n";
        }
    }
}

}  // namespace flexql
