/*
 * test_wal.cpp — Tests for Write-Ahead Log
 */

#include "server/storage/wal.h"
#include <iostream>
#include <cassert>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    std::cout << "  [TEST] " << #name << "... "; \
    try { test_##name(); tests_passed++; std::cout << "PASS\n"; } \
    catch (const std::exception& e) { tests_failed++; std::cout << "FAIL: " << e.what() << "\n"; }

#define ASSERT(cond) \
    if (!(cond)) throw std::runtime_error("Assertion failed: " #cond)

// ─── Test helpers ──────────────────────────────────────────────────────

static std::string test_dir() {
    std::string dir = "/tmp/flexql_test_wal";
    fs::create_directories(dir);
    return dir;
}

static void cleanup(const std::string& path) {
    if (fs::exists(path)) fs::remove(path);
}

// ─── Tests ─────────────────────────────────────────────────────────────

void test_crc32_basic() {
    const char* data = "Hello, FlexQL!";
    uint32_t crc = flexql::crc32_compute(data, strlen(data));
    ASSERT(crc != 0);
    // Same input should give same output
    uint32_t crc2 = flexql::crc32_compute(data, strlen(data));
    ASSERT(crc == crc2);
    // Different input should give different output
    const char* data2 = "Hello, FlexQL?";
    uint32_t crc3 = flexql::crc32_compute(data2, strlen(data2));
    ASSERT(crc != crc3);
}

void test_wal_write_read() {
    std::string path = test_dir() + "/test_write_read.wal";
    cleanup(path);

    // Write 3 records
    {
        flexql::WALWriter writer(path);
        ASSERT(writer.is_open());
        uint64_t lsn1 = writer.append("CREATE TABLE t1 (id INT, name VARCHAR(50))");
        uint64_t lsn2 = writer.append("INSERT INTO t1 VALUES (1, 'Alice')");
        uint64_t lsn3 = writer.append("INSERT INTO t1 VALUES (2, 'Bob')");
        ASSERT(lsn1 == 1);
        ASSERT(lsn2 == 2);
        ASSERT(lsn3 == 3);
    }

    // Read back
    {
        flexql::WALReader reader(path);
        ASSERT(reader.is_open());
        auto records = reader.read_all();
        ASSERT(records.size() == 3);
        ASSERT(records[0].lsn == 1);
        ASSERT(records[0].sql == "CREATE TABLE t1 (id INT, name VARCHAR(50))");
        ASSERT(records[1].lsn == 2);
        ASSERT(records[1].sql == "INSERT INTO t1 VALUES (1, 'Alice')");
        ASSERT(records[2].lsn == 3);
        ASSERT(records[2].sql == "INSERT INTO t1 VALUES (2, 'Bob')");
    }

    cleanup(path);
}

void test_wal_replay_callback() {
    std::string path = test_dir() + "/test_replay.wal";
    cleanup(path);

    {
        flexql::WALWriter writer(path);
        writer.append("SQL1");
        writer.append("SQL2");
        writer.append("SQL3");
        writer.append("SQL4");
        writer.append("SQL5");
    }

    // Replay and count
    flexql::WALReader reader(path);
    int count = 0;
    reader.replay([&](const flexql::WALRecord& rec) -> bool {
        count++;
        return true;
    });
    ASSERT(count == 5);

    cleanup(path);
}

void test_wal_empty_file() {
    std::string path = test_dir() + "/test_empty.wal";
    cleanup(path);

    // Create empty file
    { std::ofstream f(path); }

    flexql::WALReader reader(path);
    auto records = reader.read_all();
    ASSERT(records.size() == 0);

    cleanup(path);
}

void test_wal_nonexistent_file() {
    flexql::WALReader reader("/tmp/flexql_nonexistent_12345.wal");
    auto records = reader.read_all();
    ASSERT(records.size() == 0);
}

void test_wal_truncate() {
    std::string path = test_dir() + "/test_truncate.wal";
    cleanup(path);

    {
        flexql::WALWriter writer(path);
        writer.append("SQL1");
        writer.append("SQL2");
        writer.append("SQL3");
        writer.truncate();

        // Write new records after truncate
        writer.append("SQL4");
    }

    flexql::WALReader reader(path);
    auto records = reader.read_all();
    // After truncate + 1 new write, should have 1 record
    ASSERT(records.size() == 1);
    ASSERT(records[0].sql == "SQL4");
    // LSN should be 4 (continues from before truncate)
    ASSERT(records[0].lsn == 4);

    cleanup(path);
}

void test_wal_lsn_continuity_after_reopen() {
    std::string path = test_dir() + "/test_reopen.wal";
    cleanup(path);

    // Write 3 records, close
    {
        flexql::WALWriter writer(path);
        writer.append("A");
        writer.append("B");
        writer.append("C");
    }

    // Reopen and write more
    {
        flexql::WALWriter writer(path);
        uint64_t lsn = writer.append("D");
        ASSERT(lsn == 4);
    }

    // Verify all 4 records
    flexql::WALReader reader(path);
    auto records = reader.read_all();
    ASSERT(records.size() == 4);
    ASSERT(records[3].sql == "D");
    ASSERT(records[3].lsn == 4);

    cleanup(path);
}

void test_wal_large_payload() {
    std::string path = test_dir() + "/test_large.wal";
    cleanup(path);

    // Build a large SQL string (1 MB)
    std::string big_sql(1024 * 1024, 'X');

    {
        flexql::WALWriter writer(path);
        writer.append(big_sql);
    }

    flexql::WALReader reader(path);
    auto records = reader.read_all();
    ASSERT(records.size() == 1);
    ASSERT(records[0].sql.size() == 1024 * 1024);
    ASSERT(records[0].sql == big_sql);

    cleanup(path);
}

void test_wal_corrupted_record() {
    std::string path = test_dir() + "/test_corrupt.wal";
    cleanup(path);

    // Write 2 valid records
    {
        flexql::WALWriter writer(path);
        writer.append("GOOD1");
        writer.append("GOOD2");
    }

    // Corrupt the file by appending garbage
    {
        std::ofstream f(path, std::ios::binary | std::ios::app);
        const char garbage[] = "DEADBEEF";
        f.write(garbage, sizeof(garbage));
    }

    // Should read 2 valid records and stop at corruption
    flexql::WALReader reader(path);
    auto records = reader.read_all();
    ASSERT(records.size() == 2);
    ASSERT(records[0].sql == "GOOD1");
    ASSERT(records[1].sql == "GOOD2");

    cleanup(path);
}

// ─── Main ──────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== FlexQL WAL Tests ===\n\n";

    TEST(crc32_basic)
    TEST(wal_write_read)
    TEST(wal_replay_callback)
    TEST(wal_empty_file)
    TEST(wal_nonexistent_file)
    TEST(wal_truncate)
    TEST(wal_lsn_continuity_after_reopen)
    TEST(wal_large_payload)
    TEST(wal_corrupted_record)

    std::cout << "\n--- Results: " << tests_passed << " passed, "
              << tests_failed << " failed ---\n";

    // Cleanup test dir
    fs::remove_all("/tmp/flexql_test_wal");

    return tests_failed > 0 ? 1 : 0;
}
