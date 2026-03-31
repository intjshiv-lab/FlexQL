/*
 * schema.cpp
 */

#include "schema.h"
#include <algorithm>

namespace flexql {

void Schema::add_column(const std::string& name, DataType type, uint16_t max_len) {
    size_t idx = columns_.size();
    uint16_t ml = (type == DataType::VARCHAR) ? max_len : 0;
    columns_.emplace_back(name, type, ml);
    col_index_[name] = idx;

    // Compute offset: sum of all previous column sizes
    uint32_t offset = row_size_;
    offsets_.push_back(offset);
    row_size_ += columns_.back().storage_size();
}

int Schema::find_column(const std::string& name) const {
    auto it = col_index_.find(name);
    if (it == col_index_.end()) return -1;
    return static_cast<int>(it->second);
}

bool Schema::validate_row(const Row& values, std::string& error) const {
    if (values.size() != columns_.size()) {
        error = "Column count mismatch: expected " +
                std::to_string(columns_.size()) + ", got " +
                std::to_string(values.size());
        return false;
    }

    for (size_t i = 0; i < columns_.size(); i++) {
        if (values[i].type != columns_[i].type) {
            error = "Type mismatch for column '" + columns_[i].name +
                    "': expected " + datatype_to_string(columns_[i].type) +
                    ", got " + datatype_to_string(values[i].type);
            return false;
        }
        if (columns_[i].type == DataType::VARCHAR &&
            values[i].str_val.size() > columns_[i].max_len) {
            error = "VARCHAR overflow for column '" + columns_[i].name +
                    "': max " + std::to_string(columns_[i].max_len) +
                    ", got " + std::to_string(values[i].str_val.size());
            return false;
        }
    }
    return true;
}

}  // namespace flexql
