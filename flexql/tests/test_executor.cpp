/*
 * ============================================================================
 *  FlexQL — Executor Tests
 * ============================================================================
 */

#include "server/executor/executor.h"
#include <cassert>
#include <iostream>

using namespace flexql;

void test_create_table() {
    Executor exec;
    auto r = exec.execute_sql("CREATE TABLE users (id INT, name VARCHAR(50), score DECIMAL)");
    assert(r.success);
    assert(r.message.find("created") != std::string::npos);
    std::cout << "[PASS] test_create_table\n";
}

void test_create_duplicate_table() {
    Executor exec;
    exec.execute_sql("CREATE TABLE t1 (id INT)");
    auto r = exec.execute_sql("CREATE TABLE t1 (id INT)");
    assert(!r.success);
    assert(r.error.find("already exists") != std::string::npos);
    std::cout << "[PASS] test_create_duplicate_table\n";
}

void test_insert_and_select() {
    Executor exec;
    exec.execute_sql("CREATE TABLE emp (id INT, name VARCHAR(50), salary DECIMAL)");

    auto r1 = exec.execute_sql("INSERT INTO emp VALUES (1, 'Alice', 75000.50)");
    assert(r1.success);

    auto r2 = exec.execute_sql("INSERT INTO emp VALUES (2, 'Bob', 60000.00)");
    assert(r2.success);

    auto r3 = exec.execute_sql("SELECT * FROM emp");
    assert(r3.success);
    assert(r3.rows.size() == 2);
    assert(r3.column_names.size() == 3);
    std::cout << "[PASS] test_insert_and_select\n";
}

void test_select_columns() {
    Executor exec;
    exec.execute_sql("CREATE TABLE t (a INT, b VARCHAR(20), c DECIMAL)");
    exec.execute_sql("INSERT INTO t VALUES (1, 'hello', 3.14)");

    auto r = exec.execute_sql("SELECT b, c FROM t");
    assert(r.success);
    assert(r.column_names.size() == 2);
    assert(r.column_names[0] == "B");
    assert(r.column_names[1] == "C");
    assert(r.rows[0].size() == 2);
    std::cout << "[PASS] test_select_columns\n";
}

void test_where_clause() {
    Executor exec;
    exec.execute_sql("CREATE TABLE items (id INT, price DECIMAL)");
    for (int i = 1; i <= 20; ++i) {
        exec.execute_sql("INSERT INTO items VALUES (" + std::to_string(i) + ", " +
                         std::to_string(i * 10.0) + ")");
    }

    auto r = exec.execute_sql("SELECT * FROM items WHERE price > 150");
    assert(r.success);
    assert(r.rows.size() == 5);  // 160, 170, 180, 190, 200

    auto r2 = exec.execute_sql("SELECT * FROM items WHERE id = 10");
    assert(r2.success);
    assert(r2.rows.size() == 1);
    assert(r2.rows[0][0].int_val == 10);

    std::cout << "[PASS] test_where_clause\n";
}

void test_where_le_ne() {
    Executor exec;
    exec.execute_sql("CREATE TABLE nums (val INT)");
    for (int i = 1; i <= 5; ++i)
        exec.execute_sql("INSERT INTO nums VALUES (" + std::to_string(i) + ")");

    auto r1 = exec.execute_sql("SELECT * FROM nums WHERE val <= 3");
    assert(r1.success);
    assert(r1.rows.size() == 3);

    auto r2 = exec.execute_sql("SELECT * FROM nums WHERE val != 3");
    assert(r2.success);
    assert(r2.rows.size() == 4);

    std::cout << "[PASS] test_where_le_ne\n";
}

void test_inner_join() {
    Executor exec;
    exec.execute_sql("CREATE TABLE departments (id INT, dept_name VARCHAR(50))");
    exec.execute_sql("INSERT INTO departments VALUES (1, 'Engineering')");
    exec.execute_sql("INSERT INTO departments VALUES (2, 'Marketing')");

    exec.execute_sql("CREATE TABLE employees (id INT, name VARCHAR(50), dept_id INT)");
    exec.execute_sql("INSERT INTO employees VALUES (1, 'Alice', 1)");
    exec.execute_sql("INSERT INTO employees VALUES (2, 'Bob', 2)");
    exec.execute_sql("INSERT INTO employees VALUES (3, 'Charlie', 1)");

    auto r = exec.execute_sql(
        "SELECT * FROM employees INNER JOIN departments ON employees.dept_id = departments.id");
    assert(r.success);
    assert(r.rows.size() == 3);
    std::cout << "[PASS] test_inner_join (" << r.rows.size() << " rows)\n";
}

void test_all_data_types() {
    Executor exec;
    exec.execute_sql("CREATE TABLE types_test (i INT, d DECIMAL, v VARCHAR(100), dt DATETIME)");
    auto r = exec.execute_sql("INSERT INTO types_test VALUES (42, 3.14159, 'hello world', 1700000000)");
    assert(r.success);

    auto r2 = exec.execute_sql("SELECT * FROM types_test");
    assert(r2.success);
    assert(r2.rows.size() == 1);
    assert(r2.rows[0][0].int_val == 42);
    assert(r2.rows[0][2].str_val == "hello world");
    assert(r2.rows[0][3].dt_val == 1700000000);
    std::cout << "[PASS] test_all_data_types (INT, DECIMAL, VARCHAR, DATETIME)\n";
}

void test_insert_table_not_found() {
    Executor exec;
    auto r = exec.execute_sql("INSERT INTO nonexistent VALUES (1)");
    assert(!r.success);
    assert(r.error.find("not found") != std::string::npos);
    std::cout << "[PASS] test_insert_table_not_found\n";
}

void test_column_mismatch() {
    Executor exec;
    exec.execute_sql("CREATE TABLE t2 (a INT, b INT)");
    auto r = exec.execute_sql("INSERT INTO t2 VALUES (1)");
    assert(!r.success);
    assert(r.error.find("mismatch") != std::string::npos);
    std::cout << "[PASS] test_column_mismatch\n";
}

void test_parse_error() {
    Executor exec;
    auto r = exec.execute_sql("INVALID QUERY");
    assert(!r.success);
    assert(r.error.find("Parse error") != std::string::npos);
    std::cout << "[PASS] test_parse_error\n";
}

void test_cache_hit() {
    Executor exec;
    exec.execute_sql("CREATE TABLE cache_test (id INT, val DECIMAL)");
    for (int i = 1; i <= 10; ++i)
        exec.execute_sql("INSERT INTO cache_test VALUES (" +
                         std::to_string(i) + ", " + std::to_string(i * 1.1) + ")");

    // First query (cache miss)
    auto r1 = exec.execute_sql("SELECT * FROM cache_test WHERE id = 5");
    assert(r1.success);
    assert(r1.rows.size() == 1);

    // Second identical query (should hit cache)
    auto r2 = exec.execute_sql("SELECT * FROM cache_test WHERE id = 5");
    assert(r2.success);
    assert(r2.rows.size() == 1);

    assert(exec.cache().hits() >= 1);
    std::cout << "[PASS] test_cache_hit (hits: " << exec.cache().hits() << ")\n";
}

void test_cache_invalidation() {
    Executor exec;
    exec.execute_sql("CREATE TABLE cinv (id INT)");
    exec.execute_sql("INSERT INTO cinv VALUES (1)");

    exec.execute_sql("SELECT * FROM cinv");

    // Insert invalidates cache
    exec.execute_sql("INSERT INTO cinv VALUES (2)");

    auto r = exec.execute_sql("SELECT * FROM cinv");
    assert(r.success);
    assert(r.rows.size() == 2);
    std::cout << "[PASS] test_cache_invalidation\n";
}

int main() {
    std::cout << "=== Executor Tests ===\n";
    test_create_table();
    test_create_duplicate_table();
    test_insert_and_select();
    test_select_columns();
    test_where_clause();
    test_where_le_ne();
    test_inner_join();
    test_all_data_types();
    test_insert_table_not_found();
    test_column_mismatch();
    test_parse_error();
    test_cache_hit();
    test_cache_invalidation();
    std::cout << "All executor tests passed!\n";
    return 0;
}
