/*
 * ============================================================================
 *  FlexQL — Integration Tests
 *  End-to-end SQL workflow through the executor (no network).
 * ============================================================================
 */

#include "server/executor/executor.h"
#include "common.h"
#include <cassert>
#include <iostream>
#include <chrono>
#include <thread>

using namespace flexql;

void test_full_workflow() {
    Executor exec;

    // CREATE
    auto r = exec.execute_sql("CREATE TABLE products (id INT, name VARCHAR(100), price DECIMAL, created DATETIME)");
    assert(r.success);

    // INSERT multiple rows
    exec.execute_sql("INSERT INTO products VALUES (1, 'Widget', 9.99, 1700000000)");
    exec.execute_sql("INSERT INTO products VALUES (2, 'Gadget', 24.50, 1700000100)");
    exec.execute_sql("INSERT INTO products VALUES (3, 'Gizmo', 5.75, 1700000200)");
    exec.execute_sql("INSERT INTO products VALUES (4, 'Thingamajig', 49.99, 1700000300)");
    exec.execute_sql("INSERT INTO products VALUES (5, 'Doohickey', 12.00, 1700000400)");

    // SELECT ALL
    auto all = exec.execute_sql("SELECT * FROM products");
    assert(all.success);
    assert(all.rows.size() == 5);

    // SELECT with projection
    auto proj = exec.execute_sql("SELECT name, price FROM products");
    assert(proj.success);
    assert(proj.column_names.size() == 2);
    assert(proj.rows[0].size() == 2);

    // WHERE =
    auto eq = exec.execute_sql("SELECT * FROM products WHERE id = 3");
    assert(eq.success && eq.rows.size() == 1);
    assert(eq.rows[0][1].str_val == "Gizmo");

    // WHERE >
    auto gt = exec.execute_sql("SELECT * FROM products WHERE price > 10");
    assert(gt.success && gt.rows.size() == 3);  // Gadget, Thingamajig, Doohickey

    // WHERE <
    auto lt = exec.execute_sql("SELECT * FROM products WHERE price < 10");
    assert(lt.success && lt.rows.size() == 2);  // Widget, Gizmo

    // WHERE >=
    auto ge = exec.execute_sql("SELECT * FROM products WHERE price >= 12.00");
    assert(ge.success && ge.rows.size() == 3);

    // WHERE <=
    auto le = exec.execute_sql("SELECT * FROM products WHERE price <= 9.99");
    assert(le.success && le.rows.size() == 2);

    // WHERE !=
    auto ne = exec.execute_sql("SELECT * FROM products WHERE id != 1");
    assert(ne.success && ne.rows.size() == 4);

    std::cout << "[PASS] test_full_workflow\n";
}

void test_join_workflow() {
    Executor exec;

    exec.execute_sql("CREATE TABLE categories (cat_id INT, cat_name VARCHAR(50))");
    exec.execute_sql("INSERT INTO categories VALUES (1, 'Electronics')");
    exec.execute_sql("INSERT INTO categories VALUES (2, 'Books')");
    exec.execute_sql("INSERT INTO categories VALUES (3, 'Clothing')");

    exec.execute_sql("CREATE TABLE items (item_id INT, item_name VARCHAR(50), cat_id INT)");
    exec.execute_sql("INSERT INTO items VALUES (1, 'Laptop', 1)");
    exec.execute_sql("INSERT INTO items VALUES (2, 'Novel', 2)");
    exec.execute_sql("INSERT INTO items VALUES (3, 'Shirt', 3)");
    exec.execute_sql("INSERT INTO items VALUES (4, 'Phone', 1)");
    exec.execute_sql("INSERT INTO items VALUES (5, 'Textbook', 2)");

    auto r = exec.execute_sql(
        "SELECT * FROM items INNER JOIN categories ON items.cat_id = categories.cat_id");
    assert(r.success);
    assert(r.rows.size() == 5);

    std::cout << "[PASS] test_join_workflow (" << r.rows.size() << " joined rows)\n";
}

void test_large_table() {
    Executor exec;
    exec.execute_sql("CREATE TABLE big (id INT, val DECIMAL)");

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 1; i <= 10000; ++i) {
        exec.execute_sql("INSERT INTO big VALUES (" + std::to_string(i) + ", " +
                         std::to_string(i * 0.01) + ")");
    }
    auto end = std::chrono::high_resolution_clock::now();
    double insert_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Full scan
    start = std::chrono::high_resolution_clock::now();
    auto all = exec.execute_sql("SELECT * FROM big");
    end = std::chrono::high_resolution_clock::now();
    double scan_ms = std::chrono::duration<double, std::milli>(end - start).count();

    assert(all.success && all.rows.size() == 10000);

    // Point lookup
    start = std::chrono::high_resolution_clock::now();
    auto point = exec.execute_sql("SELECT * FROM big WHERE id = 5000");
    end = std::chrono::high_resolution_clock::now();
    double point_ms = std::chrono::duration<double, std::milli>(end - start).count();

    assert(point.success && point.rows.size() == 1);

    // Range scan
    start = std::chrono::high_resolution_clock::now();
    auto range = exec.execute_sql("SELECT * FROM big WHERE id > 9000");
    end = std::chrono::high_resolution_clock::now();
    double range_ms = std::chrono::duration<double, std::milli>(end - start).count();

    assert(range.success && range.rows.size() == 1000);

    std::cout << "[PASS] test_large_table (10K rows)\n";
    std::cout << "  Insert 10K: " << insert_ms << " ms\n";
    std::cout << "  Full scan:  " << scan_ms << " ms\n";
    std::cout << "  Point lookup: " << point_ms << " ms\n";
    std::cout << "  Range scan (1K): " << range_ms << " ms\n";
}

void test_varchar_where() {
    Executor exec;
    exec.execute_sql("CREATE TABLE names (id INT, name VARCHAR(50))");
    exec.execute_sql("INSERT INTO names VALUES (1, 'Alice')");
    exec.execute_sql("INSERT INTO names VALUES (2, 'Bob')");
    exec.execute_sql("INSERT INTO names VALUES (3, 'Charlie')");

    auto r = exec.execute_sql("SELECT * FROM names WHERE name = 'Bob'");
    assert(r.success);
    assert(r.rows.size() == 1);
    assert(r.rows[0][0].int_val == 2);
    std::cout << "[PASS] test_varchar_where\n";
}

void test_datetime_type() {
    Executor exec;
    exec.execute_sql("CREATE TABLE events (id INT, ts DATETIME)");
    exec.execute_sql("INSERT INTO events VALUES (1, 1700000000)");
    exec.execute_sql("INSERT INTO events VALUES (2, 1700100000)");
    exec.execute_sql("INSERT INTO events VALUES (3, 1700200000)");

    auto r = exec.execute_sql("SELECT * FROM events WHERE ts > 1700050000");
    assert(r.success);
    assert(r.rows.size() == 2);

    // Verify DATETIME values are correct
    auto all = exec.execute_sql("SELECT * FROM events");
    assert(all.rows[0][1].dt_val == 1700000000);
    std::cout << "[PASS] test_datetime_type\n";
}

void test_error_handling() {
    Executor exec;

    // Unknown table
    auto r1 = exec.execute_sql("SELECT * FROM ghost");
    assert(!r1.success);

    // Bad SQL
    auto r2 = exec.execute_sql("SELEC * FRO TABLE");
    assert(!r2.success);

    // Insert into unknown table
    auto r3 = exec.execute_sql("INSERT INTO ghost VALUES (1)");
    assert(!r3.success);

    // Wrong column count
    exec.execute_sql("CREATE TABLE t (a INT, b INT)");
    auto r4 = exec.execute_sql("INSERT INTO t VALUES (1)");
    assert(!r4.success);

    std::cout << "[PASS] test_error_handling\n";
}

void test_case_insensitivity() {
    Executor exec;
    exec.execute_sql("CREATE TABLE MyTable (Id INT, Name VARCHAR(30))");
    exec.execute_sql("INSERT INTO MYTABLE VALUES (1, 'Test')");

    auto r = exec.execute_sql("select * from mytable where id = 1");
    assert(r.success);
    assert(r.rows.size() == 1);
    std::cout << "[PASS] test_case_insensitivity\n";
}

int main() {
    std::cout << "=== Integration Tests ===\n";
    test_full_workflow();
    test_join_workflow();
    test_large_table();
    test_varchar_where();
    test_datetime_type();
    test_error_handling();
    test_case_insensitivity();
    std::cout << "All integration tests passed!\n";
    return 0;
}
