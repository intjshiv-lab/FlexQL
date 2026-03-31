/*
 * test_snapshot.cpp — Tests for Snapshot save/load
 */

#include "server/storage/snapshot.h"
#include "server/storage/table.h"
#include "server/storage/schema.h"
#include "common.h"
#include <iostream>
#include <cassert>
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

static std::string test_dir() {
    std::string dir = "/tmp/flexql_test_snapshot";
    fs::create_directories(dir);
    return dir;
}

// ─── Tests ─────────────────────────────────────────────────────────────

void test_snapshot_empty_db() {
    std::string path = test_dir() + "/empty.dat";

    std::unordered_map<std::string, std::unique_ptr<flexql::Table>> tables;
    std::string err;
    bool ok = flexql::Snapshot::save(path, tables, 0, err);
    ASSERT(ok);

    auto result = flexql::Snapshot::load(path);
    ASSERT(result.success);
    ASSERT(result.tables.empty());
    ASSERT(result.last_wal_lsn == 0);
}

void test_snapshot_single_table() {
    std::string path = test_dir() + "/single.dat";

    // Create a table with data
    flexql::Schema schema;
    schema.add_column("ID", flexql::DataType::INT);
    schema.add_column("NAME", flexql::DataType::VARCHAR, 50);
    schema.add_column("GPA", flexql::DataType::DECIMAL);

    auto table = std::make_unique<flexql::Table>("STUDENTS", schema, 0);

    flexql::Row r1 = {
        flexql::Value::make_int(1),
        flexql::Value::make_varchar("Alice"),
        flexql::Value::make_decimal(9.5)
    };
    flexql::Row r2 = {
        flexql::Value::make_int(2),
        flexql::Value::make_varchar("Bob"),
        flexql::Value::make_decimal(8.2)
    };

    std::string err;
    ASSERT(table->insert(r1, err));
    ASSERT(table->insert(r2, err));

    // Save
    std::unordered_map<std::string, std::unique_ptr<flexql::Table>> tables;
    tables["STUDENTS"] = std::move(table);
    ASSERT(flexql::Snapshot::save(path, tables, 42, err));

    // Load
    auto result = flexql::Snapshot::load(path);
    ASSERT(result.success);
    ASSERT(result.last_wal_lsn == 42);
    ASSERT(result.tables.size() == 1);
    ASSERT(result.tables.count("STUDENTS") == 1);

    auto& loaded = result.tables["STUDENTS"];
    ASSERT(loaded->row_count() == 2);
    ASSERT(loaded->schema().num_columns() == 3);

    // Verify data
    auto rows = loaded->scan_all();
    ASSERT(rows.size() == 2);
    ASSERT(rows[0][0].int_val == 1);
    ASSERT(rows[0][1].str_val == "Alice");
    ASSERT(rows[1][0].int_val == 2);
    ASSERT(rows[1][1].str_val == "Bob");
}

void test_snapshot_multiple_tables() {
    std::string path = test_dir() + "/multi.dat";

    // Table 1
    flexql::Schema s1;
    s1.add_column("ID", flexql::DataType::INT);
    s1.add_column("VAL", flexql::DataType::DECIMAL);
    auto t1 = std::make_unique<flexql::Table>("T1", s1, 0);

    std::string err;
    flexql::Row r1 = { flexql::Value::make_int(10), flexql::Value::make_decimal(3.14) };
    t1->insert(r1, err);

    // Table 2
    flexql::Schema s2;
    s2.add_column("KEY", flexql::DataType::INT);
    s2.add_column("TEXT", flexql::DataType::VARCHAR, 100);
    auto t2 = std::make_unique<flexql::Table>("T2", s2, 0);

    flexql::Row r2 = { flexql::Value::make_int(1), flexql::Value::make_varchar("hello") };
    flexql::Row r3 = { flexql::Value::make_int(2), flexql::Value::make_varchar("world") };
    t2->insert(r2, err);
    t2->insert(r3, err);

    // Save
    std::unordered_map<std::string, std::unique_ptr<flexql::Table>> tables;
    tables["T1"] = std::move(t1);
    tables["T2"] = std::move(t2);
    ASSERT(flexql::Snapshot::save(path, tables, 100, err));

    // Load
    auto result = flexql::Snapshot::load(path);
    ASSERT(result.success);
    ASSERT(result.tables.size() == 2);
    ASSERT(result.tables["T1"]->row_count() == 1);
    ASSERT(result.tables["T2"]->row_count() == 2);
    ASSERT(result.last_wal_lsn == 100);
}

void test_snapshot_with_datetime() {
    std::string path = test_dir() + "/datetime.dat";

    flexql::Schema schema;
    schema.add_column("ID", flexql::DataType::INT);
    schema.add_column("TS", flexql::DataType::DATETIME);
    auto table = std::make_unique<flexql::Table>("EVENTS", schema, 0);

    std::string err;
    flexql::Row r = { flexql::Value::make_int(1), flexql::Value::make_datetime(1741842600) };
    table->insert(r, err);

    std::unordered_map<std::string, std::unique_ptr<flexql::Table>> tables;
    tables["EVENTS"] = std::move(table);
    ASSERT(flexql::Snapshot::save(path, tables, 5, err));

    auto result = flexql::Snapshot::load(path);
    ASSERT(result.success);
    auto rows = result.tables["EVENTS"]->scan_all();
    ASSERT(rows.size() == 1);
    ASSERT(rows[0][1].dt_val == 1741842600);
}

void test_snapshot_nonexistent_file() {
    auto result = flexql::Snapshot::load("/tmp/nonexistent_snapshot_xyz.dat");
    ASSERT(!result.success);
}

void test_snapshot_round_trip_10k_rows() {
    std::string path = test_dir() + "/big.dat";

    flexql::Schema schema;
    schema.add_column("ID", flexql::DataType::INT);
    schema.add_column("NAME", flexql::DataType::VARCHAR, 20);
    auto table = std::make_unique<flexql::Table>("BIG", schema, 0);

    std::string err;
    for (int i = 0; i < 10000; i++) {
        flexql::Row r = {
            flexql::Value::make_int(i),
            flexql::Value::make_varchar("row_" + std::to_string(i))
        };
        ASSERT(table->insert(r, err));
    }

    std::unordered_map<std::string, std::unique_ptr<flexql::Table>> tables;
    tables["BIG"] = std::move(table);
    ASSERT(flexql::Snapshot::save(path, tables, 10000, err));

    auto result = flexql::Snapshot::load(path);
    ASSERT(result.success);
    ASSERT(result.tables["BIG"]->row_count() == 10000);

    auto rows = result.tables["BIG"]->scan_all();
    ASSERT(rows.size() == 10000);
    ASSERT(rows[0][0].int_val == 0);
    ASSERT(rows[9999][0].int_val == 9999);
    ASSERT(rows[9999][1].str_val == "row_9999");
}

// ─── Main ──────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== FlexQL Snapshot Tests ===\n\n";

    TEST(snapshot_empty_db)
    TEST(snapshot_single_table)
    TEST(snapshot_multiple_tables)
    TEST(snapshot_with_datetime)
    TEST(snapshot_nonexistent_file)
    TEST(snapshot_round_trip_10k_rows)

    std::cout << "\n--- Results: " << tests_passed << " passed, "
              << tests_failed << " failed ---\n";

    fs::remove_all("/tmp/flexql_test_snapshot");
    return tests_failed > 0 ? 1 : 0;
}
