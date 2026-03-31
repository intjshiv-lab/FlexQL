/*
 * table.cpp — row storage engine
 *
 * Each row is a flat byte buffer in the arena. We memcpy fields in and
 * out based on the schema offsets. Not the prettiest but it's fast and
 * avoids any per-field heap allocation.
 *
 * The valid flag + expiry timestamp sit at the end of each row so we
 * can check TTL without reading the actual columns.
 */

#include "table.h"
#include <cstring>
#include <ctime>
#include <algorithm>

namespace flexql {

Table::Table(const std::string& name, const Schema& schema, uint32_t ttl_seconds)
    : name_(name)
    , schema_(schema)
    , ttl_seconds_(ttl_seconds)
    , arena_(ARENA_CHUNK_SIZE)
    , cached_row_size_(schema.total_row_size())
{
    pk_index_ = std::make_unique<BPTree>(schema_.pk_column().type);
    rows_.reserve(1024);  // Pre-reserve to avoid early re-allocations
}

// ─── Write a row into raw storage ──────────────────────────────────────────

void Table::write_row(uint8_t* ptr, const Row& values, int64_t expiry) {
    for (size_t i = 0; i < schema_.columns().size(); i++) {
        uint32_t offset = schema_.column_offset(i);
        const auto& col = schema_.columns()[i];
        uint8_t* dest = ptr + offset;

        switch (col.type) {
            case DataType::INT:
                std::memcpy(dest, &values[i].int_val, 4);
                break;
            case DataType::DECIMAL:
                std::memcpy(dest, &values[i].dec_val, 8);
                break;
            case DataType::VARCHAR: {
                uint16_t len = static_cast<uint16_t>(values[i].str_val.size());
                std::memcpy(dest, &len, 2);
                std::memcpy(dest + 2, values[i].str_val.data(), len);
                break;
            }
            case DataType::DATETIME:
                std::memcpy(dest, &values[i].dt_val, 8);
                break;
        }
    }
    // Write expiry timestamp
    std::memcpy(ptr + schema_.expiry_offset(), &expiry, 8);

    // Write valid flag
    uint8_t valid = 1;
    ptr[schema_.valid_offset()] = valid;
}

// ─── Read a full row from raw storage ──────────────────────────────────────

Value Table::read_value(const uint8_t* row_ptr, int col_idx) const {
    const auto& col = schema_.columns()[col_idx];
    const uint8_t* src = row_ptr + schema_.column_offset(col_idx);
    Value v;
    v.type = col.type;

    switch (col.type) {
        case DataType::INT:
            std::memcpy(&v.int_val, src, 4);
            break;
        case DataType::DECIMAL:
            std::memcpy(&v.dec_val, src, 8);
            break;
        case DataType::VARCHAR: {
            uint16_t len;
            std::memcpy(&len, src, 2);
            v.str_val.assign(reinterpret_cast<const char*>(src + 2), len);
            break;
        }
        case DataType::DATETIME:
            std::memcpy(&v.dt_val, src, 8);
            break;
    }
    return v;
}

Row Table::read_row(const uint8_t* ptr) const {
    Row row;
    row.reserve(schema_.num_columns());
    for (size_t i = 0; i < schema_.num_columns(); i++) {
        row.push_back(read_value(ptr, static_cast<int>(i)));
    }
    return row;
}

Row Table::read_row_projected(const uint8_t* ptr, const std::vector<int>& cols) const {
    Row row;
    row.reserve(cols.size());
    for (int idx : cols) {
        row.push_back(read_value(ptr, idx));
    }
    return row;
}

// ─── Row validity check ───────────────────────────────────────────────────

bool Table::is_row_valid(const uint8_t* ptr) const {
    // Check valid flag
    if (ptr[schema_.valid_offset()] == 0) return false;

    // Check expiry
    int64_t expiry;
    std::memcpy(&expiry, ptr + schema_.expiry_offset(), 8);
    if (expiry > 0) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (now > expiry) return false;
    }
    return true;
}

// ─── Compare value for WHERE ──────────────────────────────────────────────

bool Table::compare_value(const uint8_t* row_ptr, int col_idx,
                          CmpOp op, const Value& target) const {
    Value v = read_value(row_ptr, col_idx);
    switch (op) {
        case CmpOp::EQ: return v == target;
        case CmpOp::NE: return v != target;
        case CmpOp::LT: return v <  target;
        case CmpOp::GT: return v >  target;
        case CmpOp::LE: return v <= target;
        case CmpOp::GE: return v >= target;
    }
    return false;
}

// ─── INSERT ───────────────────────────────────────────────────────────────

bool Table::insert(const Row& values, std::string& error) {
    // Validate schema
    if (!schema_.validate_row(values, error)) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Allocate row storage from arena (use pre-computed size)
    uint8_t* row_ptr = static_cast<uint8_t*>(arena_.allocate(cached_row_size_));
    if (!row_ptr) {
        error = "Out of memory";
        return false;
    }

    // Compute expiry — skip time() syscall when TTL is 0
    int64_t expiry = 0;
    if (ttl_seconds_ > 0) {
        expiry = static_cast<int64_t>(std::time(nullptr)) + ttl_seconds_;
    }

    // Write row data
    write_row(row_ptr, values, expiry);

    // Record row pointer and update index
    uint64_t row_id = rows_.size();
    rows_.push_back(row_ptr);

    // Index on primary key
    const Value& pk_val = values[schema_.pk_index()];
    pk_index_->insert(pk_val, row_id);

    row_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ─── FULL TABLE SCAN ──────────────────────────────────────────────────────

std::vector<Row> Table::scan_all() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<Row> results;
    results.reserve(rows_.size());

    for (const auto* ptr : rows_) {
        if (is_row_valid(ptr)) {
            results.push_back(read_row(ptr));
        }
    }
    return results;
}

// ─── PROJECTED SCAN ───────────────────────────────────────────────────────

std::vector<Row> Table::scan_projected(const std::vector<int>& col_indices) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<Row> results;
    results.reserve(rows_.size());

    for (const auto* ptr : rows_) {
        if (is_row_valid(ptr)) {
            results.push_back(read_row_projected(ptr, col_indices));
        }
    }
    return results;
}

// ─── WHERE SCAN ───────────────────────────────────────────────────────────

std::vector<Row> Table::scan_where(int col_idx, CmpOp op, const Value& value,
                                   const std::vector<int>& project_cols) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    // Optimization: if WHERE is on the primary key, use the B+ tree index
    if (col_idx == static_cast<int>(schema_.pk_index())) {
        return index_range(op, value, project_cols);
    }

    // Full table scan with filter
    std::vector<Row> results;
    for (const auto* ptr : rows_) {
        if (is_row_valid(ptr) && compare_value(ptr, col_idx, op, value)) {
            if (project_cols.empty()) {
                results.push_back(read_row(ptr));
            } else {
                results.push_back(read_row_projected(ptr, project_cols));
            }
        }
    }
    return results;
}

// ─── INDEX LOOKUP ─────────────────────────────────────────────────────────

std::vector<Row> Table::index_lookup(const Value& pk_value) const {
    // Note: caller should hold at least a shared lock
    std::vector<Row> results;
    int64_t row_id = pk_index_->find(pk_value);
    if (row_id >= 0 && static_cast<uint64_t>(row_id) < rows_.size()) {
        const uint8_t* ptr = rows_[row_id];
        if (is_row_valid(ptr)) {
            results.push_back(read_row(ptr));
        }
    }
    return results;
}

// ─── INDEX RANGE SCAN ─────────────────────────────────────────────────────

std::vector<Row> Table::index_range(CmpOp op, const Value& pk_value,
                                    const std::vector<int>& project_cols) const {
    std::vector<Row> results;

    auto collect = [&](uint64_t row_id) -> bool {
        if (row_id < rows_.size()) {
            const uint8_t* ptr = rows_[row_id];
            if (is_row_valid(ptr)) {
                if (project_cols.empty()) {
                    results.push_back(read_row(ptr));
                } else {
                    results.push_back(read_row_projected(ptr, project_cols));
                }
            }
        }
        return true; // continue
    };

    switch (op) {
        case CmpOp::EQ: pk_index_->range_eq(pk_value, collect); break;
        case CmpOp::NE: pk_index_->range_ne(pk_value, collect); break;
        case CmpOp::LT: pk_index_->range_lt(pk_value, collect); break;
        case CmpOp::GT: pk_index_->range_gt(pk_value, collect); break;
        case CmpOp::LE: pk_index_->range_le(pk_value, collect); break;
        case CmpOp::GE: pk_index_->range_ge(pk_value, collect); break;
    }
    return results;
}

// ─── TTL CLEANUP ──────────────────────────────────────────────────────────

uint64_t Table::cleanup_expired() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    uint64_t cleaned = 0;
    int64_t now = static_cast<int64_t>(std::time(nullptr));

    for (auto* ptr : rows_) {
        if (ptr[schema_.valid_offset()] == 1) {
            int64_t expiry;
            std::memcpy(&expiry, ptr + schema_.expiry_offset(), 8);
            if (expiry > 0 && now > expiry) {
                ptr[schema_.valid_offset()] = 0;  // Mark as deleted
                cleaned++;
            }
        }
    }

    row_count_.fetch_sub(cleaned, std::memory_order_relaxed);
    return cleaned;
}

// ─── MEMORY USAGE ─────────────────────────────────────────────────────────

size_t Table::memory_usage() const {
    return arena_.total_allocated() +
           rows_.capacity() * sizeof(uint8_t*) +
           pk_index_->memory_usage();
}

}  // namespace flexql
