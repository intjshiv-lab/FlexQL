/*
 * ============================================================================
 *  FlexQL — Schema Tests
 * ============================================================================
 */

#include "server/storage/schema.h"
#include "common.h"
#include <cassert>
#include <iostream>

using namespace flexql;

void test_add_columns() {
    Schema schema;
    schema.add_column("ID", DataType::INT);
    schema.add_column("NAME", DataType::VARCHAR, 50);
    schema.add_column("SCORE", DataType::DECIMAL);
    schema.add_column("CREATED", DataType::DATETIME);

    assert(schema.columns().size() == 4);
    std::cout << "[PASS] test_add_columns\n";
}

void test_find_column() {
    Schema schema;
    schema.add_column("A", DataType::INT);
    schema.add_column("B", DataType::VARCHAR, 100);
    schema.add_column("C", DataType::DECIMAL);

    assert(schema.find_column("A") == 0);
    assert(schema.find_column("B") == 1);
    assert(schema.find_column("C") == 2);
    assert(schema.find_column("D") == -1);
    std::cout << "[PASS] test_find_column\n";
}

void test_column_offset() {
    Schema schema;
    schema.add_column("ID", DataType::INT);        // 4 bytes
    schema.add_column("VAL", DataType::DECIMAL);    // 8 bytes
    schema.add_column("NAME", DataType::VARCHAR, 32); // 2 + 32 bytes

    size_t off0 = schema.column_offset(0);
    size_t off1 = schema.column_offset(1);
    size_t off2 = schema.column_offset(2);

    assert(off0 == 0);
    assert(off1 == 4);
    assert(off2 == 12);
    std::cout << "[PASS] test_column_offset\n";
}

void test_total_row_size() {
    Schema schema;
    schema.add_column("ID", DataType::INT);         // 4
    schema.add_column("VAL", DataType::DECIMAL);     // 8

    size_t raw = 4 + 8;
    // total_row_size adds 8 (expiry) + 1 (valid flag)
    assert(schema.total_row_size() == raw + 9);
    std::cout << "[PASS] test_total_row_size\n";
}

void test_validate_row() {
    Schema schema;
    schema.add_column("ID", DataType::INT);
    schema.add_column("NAME", DataType::VARCHAR, 10);

    Row good = {Value::make_int(1), Value::make_varchar("hello")};
    std::string err;
    assert(schema.validate_row(good, err) == true);

    // Wrong number of values
    Row bad1 = {Value::make_int(1)};
    assert(schema.validate_row(bad1, err) == false);

    // Wrong type
    Row bad2 = {Value::make_varchar("x"), Value::make_int(2)};
    assert(schema.validate_row(bad2, err) == false);

    std::cout << "[PASS] test_validate_row\n";
}

int main() {
    std::cout << "=== Schema Tests ===\n";
    test_add_columns();
    test_find_column();
    test_column_offset();
    test_total_row_size();
    test_validate_row();
    std::cout << "All schema tests passed!\n";
    return 0;
}
