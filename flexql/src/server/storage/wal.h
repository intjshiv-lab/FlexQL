/*
 * wal.h — Write-Ahead Log for crash recovery
 *
 * Every mutating operation (CREATE TABLE, INSERT) is logged to disk
 * BEFORE being acknowledged to the client. On crash, we replay the
 * WAL to reconstruct the in-memory state.
 *
 * Record format:
 *   [4B magic][4B CRC32][8B LSN][4B payload_len][N bytes SQL]
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_WAL_H
#define FLEXQL_WAL_H

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <functional>

namespace flexql {

/* ─── WAL Record ───────────────────────────────────────────────────────── */
struct WALRecord {
    static constexpr uint32_t MAGIC = 0xDA7ABA5E;

    uint32_t    magic       = MAGIC;
    uint32_t    crc32       = 0;
    uint64_t    lsn         = 0;       // Log Sequence Number
    uint32_t    payload_len = 0;
    std::string sql;                   // The SQL statement

    // Serialized size (header + payload)
    size_t wire_size() const { return 4 + 4 + 8 + 4 + payload_len; }
};

/* ─── WAL Writer ───────────────────────────────────────────────────────── */
class WALWriter {
public:
    explicit WALWriter(const std::string& wal_path);
    ~WALWriter();

    // Append a SQL statement to the WAL. Returns the assigned LSN.
    // Thread-safe. Calls fsync to guarantee durability.
    uint64_t append(const std::string& sql);

    // Flush and sync to disk
    void sync();

    // Truncate the WAL (after a successful snapshot)
    void truncate();

    // Current LSN (monotonically increasing)
    uint64_t current_lsn() const { return next_lsn_; }

    // File path
    const std::string& path() const { return path_; }

    // Is the WAL open and writable?
    bool is_open() const { return file_.is_open(); }

private:
    std::string      path_;
    std::ofstream    file_;
    uint64_t         next_lsn_ = 1;
    mutable std::mutex mutex_;
};

/* ─── WAL Reader ───────────────────────────────────────────────────────── */
class WALReader {
public:
    explicit WALReader(const std::string& wal_path);
    ~WALReader() = default;

    // Read all valid records from the WAL file.
    // Stops at first corrupted or incomplete record.
    std::vector<WALRecord> read_all();

    // Read records and invoke callback for each.
    // Callback returns false to stop.
    using RecordCallback = std::function<bool(const WALRecord&)>;
    uint64_t replay(RecordCallback cb);

    // Is the file readable?
    bool is_open() const { return file_.is_open(); }

    // Number of records successfully read
    uint64_t records_read() const { return records_read_; }

private:
    bool read_record(WALRecord& record);

    std::string      path_;
    std::ifstream    file_;
    uint64_t         records_read_ = 0;
};

/* ─── CRC32 utility ────────────────────────────────────────────────────── */
uint32_t crc32_compute(const void* data, size_t len);

}  // namespace flexql

#endif /* FLEXQL_WAL_H */
