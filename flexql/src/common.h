/*
 * common.h — shared types used across the whole codebase
 *
 * DataType, Value, ColumnDef, and a few constants all live here
 * to avoid circular includes between modules.
 */

#ifndef FLEXQL_COMMON_H
#define FLEXQL_COMMON_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <stdexcept>
#include <functional>

namespace flexql {

/* ─── Column Data Types ────────────────────────────────────────────────── */
enum class DataType : uint8_t {
    INT      = 0,   // int32_t    — 4 bytes
    DECIMAL  = 1,   // double     — 8 bytes
    VARCHAR  = 2,   // variable   — 2-byte len prefix + chars
    DATETIME = 3,   // int64_t    — 8 bytes (Unix epoch seconds)
};

inline const char* datatype_to_string(DataType dt) {
    switch (dt) {
        case DataType::INT:      return "INT";
        case DataType::DECIMAL:  return "DECIMAL";
        case DataType::VARCHAR:  return "VARCHAR";
        case DataType::DATETIME: return "DATETIME";
    }
    return "UNKNOWN";
}

inline DataType string_to_datatype(const std::string& s) {
    if (s == "INT")      return DataType::INT;
    if (s == "DECIMAL")  return DataType::DECIMAL;
    if (s == "VARCHAR")  return DataType::VARCHAR;
    if (s == "DATETIME") return DataType::DATETIME;
    throw std::runtime_error("Unknown data type: " + s);
}

/* ─── Column Definition ────────────────────────────────────────────────── */
struct ColumnDef {
    std::string name;
    DataType    type;
    uint16_t    max_len;    // For VARCHAR; ignored for fixed types

    ColumnDef() : type(DataType::INT), max_len(0) {}
    ColumnDef(const std::string& n, DataType t, uint16_t ml = 0)
        : name(n), type(t), max_len(ml) {}

    // Fixed-size storage for this column
    uint32_t storage_size() const {
        switch (type) {
            case DataType::INT:      return 4;
            case DataType::DECIMAL:  return 8;
            case DataType::VARCHAR:  return 2 + max_len;  // len-prefix + chars
            case DataType::DATETIME: return 8;
        }
        return 0;
    }
};

/* ─── A single typed value ─────────────────────────────────────────────── */
struct Value {
    DataType type;
    union {
        int32_t  int_val;
        double   dec_val;
        int64_t  dt_val;      // datetime as epoch seconds
    };
    std::string str_val;      // used for VARCHAR

    Value() : type(DataType::INT), int_val(0) {}

    static Value make_int(int32_t v) {
        Value val; val.type = DataType::INT; val.int_val = v; return val;
    }
    static Value make_decimal(double v) {
        Value val; val.type = DataType::DECIMAL; val.dec_val = v; return val;
    }
    static Value make_varchar(const std::string& v) {
        Value val; val.type = DataType::VARCHAR; val.str_val = v; return val;
    }
    static Value make_datetime(int64_t v) {
        Value val; val.type = DataType::DATETIME; val.dt_val = v; return val;
    }

    // Convert to string for display / callback
    std::string to_string() const {
        switch (type) {
            case DataType::INT:      return std::to_string(int_val);
            case DataType::DECIMAL:  {
                // If it's a whole number, format without decimals
                if (dec_val == static_cast<double>(static_cast<int64_t>(dec_val))) {
                    return std::to_string(static_cast<int64_t>(dec_val));
                }
                char buf[64];
                snprintf(buf, sizeof(buf), "%.6f", dec_val);
                // Remove trailing zeros
                std::string s(buf);
                size_t dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t last = s.find_last_not_of('0');
                    if (last == dot) last++;
                    s = s.substr(0, last + 1);
                }
                return s;
            }
            case DataType::VARCHAR:  return str_val;
            case DataType::DATETIME: return std::to_string(dt_val);
        }
        return "";
    }

    // Hash for use as unordered_map key (zero-alloc JOIN)
    size_t hash() const {
        switch (type) {
            case DataType::INT:      return std::hash<int32_t>{}(int_val);
            case DataType::DECIMAL:  return std::hash<double>{}(dec_val);
            case DataType::VARCHAR:  return std::hash<std::string>{}(str_val);
            case DataType::DATETIME: return std::hash<int64_t>{}(dt_val);
        }
        return 0;
    }

    // Comparison operators (for WHERE clause evaluation)
    bool operator==(const Value& o) const {
        if (type != o.type) return false;
        switch (type) {
            case DataType::INT:      return int_val == o.int_val;
            case DataType::DECIMAL:  return dec_val == o.dec_val;
            case DataType::VARCHAR:  return str_val == o.str_val;
            case DataType::DATETIME: return dt_val  == o.dt_val;
        }
        return false;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }
    bool operator<(const Value& o)  const {
        switch (type) {
            case DataType::INT:      return int_val < o.int_val;
            case DataType::DECIMAL:  return dec_val < o.dec_val;
            case DataType::VARCHAR:  return str_val < o.str_val;
            case DataType::DATETIME: return dt_val  < o.dt_val;
        }
        return false;
    }
    bool operator>(const Value& o)  const { return o < *this; }
    bool operator<=(const Value& o) const { return !(o < *this); }
    bool operator>=(const Value& o) const { return !(*this < o); }
};

/* ─── Row: a vector of Values ──────────────────────────────────────────── */
using Row = std::vector<Value>;

}  // namespace flexql (temporarily close for std::hash specialization)

/* ─── std::hash specialization for Value (used in hash joins) ──────────── */
namespace std {
template<> struct hash<flexql::Value> {
    size_t operator()(const flexql::Value& v) const { return v.hash(); }
};
}

namespace flexql {  // reopen

/* ─── Performance Timer ────────────────────────────────────────────────── */
struct PerfTimer {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point start_time;

    void start()  { start_time = Clock::now(); }
    double elapsed_us() const {
        auto now = Clock::now();
        return std::chrono::duration<double, std::micro>(now - start_time).count();
    }
    double elapsed_ms() const { return elapsed_us() / 1000.0; }
    double elapsed_s()  const { return elapsed_us() / 1000000.0; }
};

/* ─── Constants ────────────────────────────────────────────────────────── */
constexpr uint16_t DEFAULT_VARCHAR_MAX    = 255;
constexpr uint32_t DEFAULT_TTL_SECONDS    = 3600;   // 1 hour
constexpr uint32_t ARENA_CHUNK_SIZE       = 4 << 20; // 4 MB chunk size
constexpr uint16_t BPTREE_ORDER           = 256;
constexpr uint32_t LRU_CACHE_CAPACITY     = 1024;
constexpr uint16_t DEFAULT_PORT           = 9876;
constexpr uint16_t THREAD_POOL_SIZE       = 8;

}  // namespace flexql

#endif /* FLEXQL_COMMON_H */
