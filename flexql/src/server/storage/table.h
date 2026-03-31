/*
 * table.h — in-memory table with arena storage, B+ tree index, and TTL
 */

#ifndef FLEXQL_TABLE_H
#define FLEXQL_TABLE_H

#include "common.h"
#include "schema.h"
#include "arena.h"
#include "server/index/bptree.h"
#include <string>
#include <vector>
#include <shared_mutex>
#include <atomic>

namespace flexql {

class Table {
public:
    explicit Table(const std::string& name, const Schema& schema,
                   uint32_t ttl_seconds = DEFAULT_TTL_SECONDS);
    ~Table() = default;

    // Non-copyable
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    // ─── Accessors ─────────────────────────────────────────────────────
    const std::string& name()       const { return name_; }
    const Schema&      schema()     const { return schema_; }
    uint64_t           row_count()  const { return row_count_.load(); }
    uint32_t           ttl()        const { return ttl_seconds_; }

    // ─── Insert a row (thread-safe) ────────────────────────────────────
    // Returns true on success; sets `error` on failure.
    bool insert(const Row& values, std::string& error);

    // ─── Full table scan ───────────────────────────────────────────────
    // Returns all non-expired rows. Each Row contains values in schema order.
    std::vector<Row> scan_all() const;

    // ─── Scan with column projection ───────────────────────────────────
    // `col_indices` specifies which columns to return.
    std::vector<Row> scan_projected(const std::vector<int>& col_indices) const;

    // ─── Scan with WHERE filter ────────────────────────────────────────
    // Returns rows where column `col_idx` satisfies `op` against `value`.
    enum class CmpOp { EQ, NE, LT, GT, LE, GE };
    std::vector<Row> scan_where(int col_idx, CmpOp op, const Value& value,
                                const std::vector<int>& project_cols = {}) const;

    // ─── Index lookup (primary key) ────────────────────────────────────
    // Point lookup: returns the row if found, or empty vector.
    std::vector<Row> index_lookup(const Value& pk_value) const;

    // Range scan via B+ tree
    std::vector<Row> index_range(CmpOp op, const Value& pk_value,
                                 const std::vector<int>& project_cols = {}) const;

    // ─── Locking for external coordination ─────────────────────────────
    std::shared_mutex& mutex() { return mutex_; }

    // ─── TTL cleanup ───────────────────────────────────────────────────
    // Remove expired rows. Returns number of rows cleaned.
    uint64_t cleanup_expired();

    // ─── Statistics ────────────────────────────────────────────────────
    size_t memory_usage() const;

private:
    // Read a Row from the raw storage at the given pointer
    Row read_row(const uint8_t* ptr) const;
    Row read_row_projected(const uint8_t* ptr, const std::vector<int>& cols) const;

    // Write a Row to the raw storage at the given pointer
    void write_row(uint8_t* ptr, const Row& values, int64_t expiry);

    // Check if a row is valid (not expired, not deleted)
    bool is_row_valid(const uint8_t* ptr) const;

    // Compare a value in a row against a target
    bool compare_value(const uint8_t* row_ptr, int col_idx,
                       CmpOp op, const Value& target) const;

    // Read a single value from a row
    Value read_value(const uint8_t* row_ptr, int col_idx) const;

    std::string              name_;
    Schema                   schema_;
    uint32_t                 ttl_seconds_;
    Arena                    arena_;
    uint32_t                 cached_row_size_;  // Pre-computed to avoid per-insert overhead
    std::vector<uint8_t*>    rows_;        // Pointers to each row in the arena
    std::atomic<uint64_t>    row_count_{0};
    mutable std::shared_mutex mutex_;

    // B+ Tree index on primary key
    std::unique_ptr<BPTree>  pk_index_;
};

}  // namespace flexql

#endif /* FLEXQL_TABLE_H */
