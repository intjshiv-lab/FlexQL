/*
 * ============================================================================
 *  FlexQL — Storage (Table) Tests
 * ============================================================================
 */

#include "server/storage/table.h"
#include "common.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

using namespace flexql;

static Schema make_schema() {
    Schema s;
    s.add_column("ID", DataType::INT);
    s.add_column("NAME", DataType::VARCHAR, 50);
    s.add_column("SCORE", DataType::DECIMAL);
    return s;
}

void test_insert_and_scan() {
    Table table("USERS", make_schema());
    Row r1 = {Value::make_int(1), Value::make_varchar("Alice"), Value::make_decimal(95.5)};
    Row r2 = {Value::make_int(2), Value::make_varchar("Bob"),   Value::make_decimal(87.3)};

    std::string err;
    assert(table.insert(r1, err));
    assert(table.insert(r2, err));

    auto all = table.scan_all();
    assert(all.size() == 2);
    assert(all[0][1].str_val == "Alice");
    assert(all[1][1].str_val == "Bob");
    std::cout << "[PASS] test_insert_and_scan\n";
}

void test_where_eq() {
    Table table("TEST", make_schema());
    std::string err;
    for (int i = 1; i <= 10; ++i) {
        Row r = {Value::make_int(i), Value::make_varchar("User" + std::to_string(i)),
                 Value::make_decimal(i * 10.0)};
        table.insert(r, err);
    }

    // WHERE ID = 5
    auto result = table.scan_where(0, Table::CmpOp::EQ, Value::make_int(5));
    assert(result.size() == 1);
    assert(result[0][0].int_val == 5);
    std::cout << "[PASS] test_where_eq\n";
}

void test_where_gt() {
    Table table("TEST", make_schema());
    std::string err;
    for (int i = 1; i <= 10; ++i) {
        Row r = {Value::make_int(i), Value::make_varchar("U"),
                 Value::make_decimal(i * 10.0)};
        table.insert(r, err);
    }

    // WHERE SCORE > 50.0
    auto result = table.scan_where(2, Table::CmpOp::GT, Value::make_decimal(50.0));
    assert(result.size() == 5);  // 60, 70, 80, 90, 100
    std::cout << "[PASS] test_where_gt\n";
}

void test_where_lt() {
    Table table("TEST", make_schema());
    std::string err;
    for (int i = 1; i <= 5; ++i) {
        Row r = {Value::make_int(i), Value::make_varchar("X"),
                 Value::make_decimal(i * 1.0)};
        table.insert(r, err);
    }

    // WHERE ID < 3
    auto result = table.scan_where(0, Table::CmpOp::LT, Value::make_int(3));
    assert(result.size() == 2);
    std::cout << "[PASS] test_where_lt\n";
}

void test_index_lookup() {
    Table table("TEST", make_schema());
    std::string err;
    for (int i = 1; i <= 100; ++i) {
        Row r = {Value::make_int(i), Value::make_varchar("Name"),
                 Value::make_decimal(i * 0.1)};
        table.insert(r, err);
    }

    auto result = table.index_lookup(Value::make_int(42));
    assert(result.size() == 1);
    assert(result[0][0].int_val == 42);
    std::cout << "[PASS] test_index_lookup\n";
}

void test_ttl_expiry() {
    Table table("TEST", make_schema(), 1);  // TTL = 1 second
    std::string err;

    Row r = {Value::make_int(1), Value::make_varchar("Temp"), Value::make_decimal(0)};
    table.insert(r, err);

    assert(table.scan_all().size() == 1);

    // Wait for expiry
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Scan should filter expired
    auto result = table.scan_all();
    assert(result.size() == 0);
    std::cout << "[PASS] test_ttl_expiry\n";
}

void test_projection() {
    Table table("TEST", make_schema());
    std::string err;
    Row r = {Value::make_int(1), Value::make_varchar("Alice"), Value::make_decimal(99.9)};
    table.insert(r, err);

    auto result = table.scan_projected({1, 2});  // NAME, SCORE only
    assert(result.size() == 1);
    assert(result[0].size() == 2);
    assert(result[0][0].str_val == "Alice");
    std::cout << "[PASS] test_projection\n";
}

void test_schema_validation() {
    Table table("TEST", make_schema());
    std::string err;

    // Wrong column count
    Row bad = {Value::make_int(1)};
    assert(!table.insert(bad, err));
    assert(!err.empty());
    std::cout << "[PASS] test_schema_validation\n";
}

void test_memory_usage() {
    Table table("TEST", make_schema());
    std::string err;
    for (int i = 0; i < 1000; ++i) {
        Row r = {Value::make_int(i), Value::make_varchar("X"), Value::make_decimal(0)};
        table.insert(r, err);
    }
    size_t mem = table.memory_usage();
    assert(mem > 0);
    std::cout << "[PASS] test_memory_usage (bytes: " << mem << ")\n";
}

int main() {
    std::cout << "=== Storage (Table) Tests ===\n";
    test_insert_and_scan();
    test_where_eq();
    test_where_gt();
    test_where_lt();
    test_index_lookup();
    test_ttl_expiry();
    test_projection();
    test_schema_validation();
    test_memory_usage();
    std::cout << "All storage tests passed!\n";
    return 0;
}
