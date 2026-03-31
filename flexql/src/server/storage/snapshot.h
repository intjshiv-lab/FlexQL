/*
 * snapshot.h — Binary snapshot of the full database state
 *
 * Serializes all tables (schema + rows) to a single file.
 * On recovery: load snapshot first, then replay WAL from last LSN.
 *
 * Format:
 *   [8B magic][8B version][8B last_wal_lsn][4B num_tables]
 *   For each table:
 *     [4B name_len][name bytes]
 *     [4B num_cols]
 *       For each column: [4B name_len][name][1B type][2B max_len]
 *     [8B num_rows]
 *       For each row: [total_row_size bytes of raw data]
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_SNAPSHOT_H
#define FLEXQL_SNAPSHOT_H

#include "common.h"
#include "server/storage/table.h"
#include "server/storage/schema.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace flexql {

class Snapshot {
public:
    static constexpr uint64_t SNAPSHOT_MAGIC   = 0x534849564451534EULL;  // "SHIVDQSN"
    static constexpr uint64_t SNAPSHOT_VERSION = 1;

    // ─── Save ─────────────────────────────────────────────────────────
    // Serialize all tables to a binary snapshot file.
    // `last_wal_lsn` is the LSN up to which this snapshot covers.
    static bool save(
        const std::string& path,
        const std::unordered_map<std::string, std::unique_ptr<Table>>& tables,
        uint64_t last_wal_lsn,
        std::string& error
    );

    // ─── Load ─────────────────────────────────────────────────────────
    // Deserialize a snapshot file and reconstruct all tables.
    // Returns the last_wal_lsn stored in the snapshot.
    struct LoadResult {
        bool     success = false;
        std::string error;
        uint64_t last_wal_lsn = 0;
        std::unordered_map<std::string, std::unique_ptr<Table>> tables;
    };

    static LoadResult load(const std::string& path);
};

}  // namespace flexql

#endif /* FLEXQL_SNAPSHOT_H */
