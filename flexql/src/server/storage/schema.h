/*
 * schema.h — table metadata (column defs, row layout, offsets)
 */

#ifndef FLEXQL_SCHEMA_H
#define FLEXQL_SCHEMA_H

#include "common.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace flexql {

class Schema {
public:
    Schema() : row_size_(0), pk_index_(0) {}

    // Add a column. First column is assumed to be the primary key.
    void add_column(const std::string& name, DataType type, uint16_t max_len = DEFAULT_VARCHAR_MAX);

    // Accessors
    const std::vector<ColumnDef>& columns()    const { return columns_; }
    size_t                        num_columns() const { return columns_.size(); }
    uint32_t                      row_size()    const { return row_size_; }
    uint32_t                      pk_index()    const { return pk_index_; }
    const ColumnDef&              pk_column()   const { return columns_[pk_index_]; }

    // Lookup column by name. Returns -1 if not found.
    int find_column(const std::string& name) const;

    // Get the byte offset of column `idx` within a row
    uint32_t column_offset(size_t idx) const {
        return (idx < offsets_.size()) ? offsets_[idx] : 0;
    }

    // Validate that a row of values matches this schema
    bool validate_row(const Row& values, std::string& error) const;

    // Total row storage: row_size + 8 (expiry timestamp) + 1 (valid flag)
    uint32_t total_row_size() const { return row_size_ + 9; }

    // Offsets for the internal metadata fields
    uint32_t expiry_offset() const { return row_size_; }
    uint32_t valid_offset()  const { return row_size_ + 8; }

private:
    std::vector<ColumnDef>                    columns_;
    std::unordered_map<std::string, size_t>   col_index_;   // name → index
    std::vector<uint32_t>                     offsets_;      // byte offset per column
    uint32_t                                  row_size_;
    uint32_t                                  pk_index_;
};

}  // namespace flexql

#endif /* FLEXQL_SCHEMA_H */
