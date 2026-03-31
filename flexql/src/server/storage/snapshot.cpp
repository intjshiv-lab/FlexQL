/*
 * snapshot.cpp — Binary snapshot implementation
 *
 * Writes/reads the complete database state to/from a single file.
 * This is the checkpoint mechanism. After a successful snapshot,
 * the WAL can be truncated (all those records are now in the snapshot).
 *
 * Author: Ramesh Choudhary
 */

#include "snapshot.h"
#include <fstream>
#include <cstring>
#include <iostream>

namespace flexql {

// ─── Helper: write binary data ────────────────────────────────────────────

static void write_u8(std::ofstream& f, uint8_t v) {
    f.write(reinterpret_cast<const char*>(&v), 1);
}
static void write_u16(std::ofstream& f, uint16_t v) {
    f.write(reinterpret_cast<const char*>(&v), 2);
}
static void write_u32(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
}
static void write_u64(std::ofstream& f, uint64_t v) {
    f.write(reinterpret_cast<const char*>(&v), 8);
}
static void write_str(std::ofstream& f, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    write_u32(f, len);
    f.write(s.data(), len);
}

// ─── Helper: read binary data ─────────────────────────────────────────────

static bool read_u8(std::ifstream& f, uint8_t& v) {
    return !!f.read(reinterpret_cast<char*>(&v), 1);
}
static bool read_u16(std::ifstream& f, uint16_t& v) {
    return !!f.read(reinterpret_cast<char*>(&v), 2);
}
static bool read_u32(std::ifstream& f, uint32_t& v) {
    return !!f.read(reinterpret_cast<char*>(&v), 4);
}
static bool read_u64(std::ifstream& f, uint64_t& v) {
    return !!f.read(reinterpret_cast<char*>(&v), 8);
}
static bool read_str(std::ifstream& f, std::string& s) {
    uint32_t len;
    if (!read_u32(f, len)) return false;
    if (len > 10 * 1024 * 1024) return false;  // sanity
    s.resize(len);
    return !!f.read(&s[0], len);
}

// ─── SAVE ─────────────────────────────────────────────────────────────────

bool Snapshot::save(
    const std::string& path,
    const std::unordered_map<std::string, std::unique_ptr<Table>>& tables,
    uint64_t last_wal_lsn,
    std::string& error)
{
    // Write to a temp file first, then rename (atomic on most filesystems)
    std::string tmp_path = path + ".tmp";
    std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        error = "Cannot open snapshot file for writing: " + tmp_path;
        return false;
    }

    // Header
    write_u64(file, SNAPSHOT_MAGIC);
    write_u64(file, SNAPSHOT_VERSION);
    write_u64(file, last_wal_lsn);
    write_u32(file, static_cast<uint32_t>(tables.size()));

    // Each table
    for (const auto& [name, table] : tables) {
        // Table name
        write_str(file, name);

        // Schema
        const Schema& schema = table->schema();
        write_u32(file, static_cast<uint32_t>(schema.num_columns()));
        for (size_t i = 0; i < schema.num_columns(); i++) {
            const auto& col = schema.columns()[i];
            write_str(file, col.name);
            write_u8(file, static_cast<uint8_t>(col.type));
            write_u16(file, col.max_len);
        }

        // Rows: scan all valid rows and serialize each as Value objects
        auto rows = table->scan_all();
        write_u64(file, static_cast<uint64_t>(rows.size()));

        for (const auto& row : rows) {
            // Write number of values
            write_u32(file, static_cast<uint32_t>(row.size()));
            for (const auto& val : row) {
                write_u8(file, static_cast<uint8_t>(val.type));
                switch (val.type) {
                    case DataType::INT:
                        file.write(reinterpret_cast<const char*>(&val.int_val), 4);
                        break;
                    case DataType::DECIMAL:
                        file.write(reinterpret_cast<const char*>(&val.dec_val), 8);
                        break;
                    case DataType::VARCHAR:
                        write_str(file, val.str_val);
                        break;
                    case DataType::DATETIME:
                        file.write(reinterpret_cast<const char*>(&val.dt_val), 8);
                        break;
                }
            }
        }
    }

    file.flush();
    file.close();

    // Atomic rename
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        error = "Cannot rename snapshot temp file to: " + path;
        return false;
    }

    return true;
}

// ─── LOAD ─────────────────────────────────────────────────────────────────

Snapshot::LoadResult Snapshot::load(const std::string& path) {
    LoadResult result;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        result.error = "Snapshot file not found: " + path;
        return result;
    }

    // Header
    uint64_t magic, version;
    if (!read_u64(file, magic) || magic != SNAPSHOT_MAGIC) {
        result.error = "Invalid snapshot magic";
        return result;
    }
    if (!read_u64(file, version) || version != SNAPSHOT_VERSION) {
        result.error = "Unsupported snapshot version: " + std::to_string(version);
        return result;
    }
    if (!read_u64(file, result.last_wal_lsn)) {
        result.error = "Cannot read WAL LSN from snapshot";
        return result;
    }

    uint32_t num_tables;
    if (!read_u32(file, num_tables)) {
        result.error = "Cannot read table count from snapshot";
        return result;
    }

    for (uint32_t t = 0; t < num_tables; t++) {
        // Table name
        std::string table_name;
        if (!read_str(file, table_name)) {
            result.error = "Cannot read table name";
            return result;
        }

        // Schema
        uint32_t num_cols;
        if (!read_u32(file, num_cols)) {
            result.error = "Cannot read column count for table: " + table_name;
            return result;
        }

        Schema schema;
        for (uint32_t c = 0; c < num_cols; c++) {
            std::string col_name;
            uint8_t type_byte;
            uint16_t max_len;
            if (!read_str(file, col_name) || !read_u8(file, type_byte) || !read_u16(file, max_len)) {
                result.error = "Cannot read column def for table: " + table_name;
                return result;
            }
            schema.add_column(col_name, static_cast<DataType>(type_byte), max_len);
        }

        auto table = std::make_unique<Table>(table_name, schema);

        // Rows
        uint64_t num_rows;
        if (!read_u64(file, num_rows)) {
            result.error = "Cannot read row count for table: " + table_name;
            return result;
        }

        for (uint64_t r = 0; r < num_rows; r++) {
            uint32_t num_vals;
            if (!read_u32(file, num_vals)) {
                result.error = "Cannot read row values for table: " + table_name;
                return result;
            }

            Row row;
            row.reserve(num_vals);
            for (uint32_t v = 0; v < num_vals; v++) {
                uint8_t type_byte;
                if (!read_u8(file, type_byte)) {
                    result.error = "Cannot read value type";
                    return result;
                }

                Value val;
                val.type = static_cast<DataType>(type_byte);
                switch (val.type) {
                    case DataType::INT:
                        if (!file.read(reinterpret_cast<char*>(&val.int_val), 4)) {
                            result.error = "Cannot read INT value";
                            return result;
                        }
                        break;
                    case DataType::DECIMAL:
                        if (!file.read(reinterpret_cast<char*>(&val.dec_val), 8)) {
                            result.error = "Cannot read DECIMAL value";
                            return result;
                        }
                        break;
                    case DataType::VARCHAR:
                        if (!read_str(file, val.str_val)) {
                            result.error = "Cannot read VARCHAR value";
                            return result;
                        }
                        break;
                    case DataType::DATETIME:
                        if (!file.read(reinterpret_cast<char*>(&val.dt_val), 8)) {
                            result.error = "Cannot read DATETIME value";
                            return result;
                        }
                        break;
                }
                row.push_back(std::move(val));
            }

            std::string err;
            if (!table->insert(row, err)) {
                result.error = "Cannot insert row during snapshot load: " + err;
                return result;
            }
        }

        result.tables[table_name] = std::move(table);
    }

    result.success = true;
    return result;
}

}  // namespace flexql
