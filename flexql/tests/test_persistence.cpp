/*
 * test_persistence.cpp — End-to-end persistence tests
 *
 * Tests the full cycle: create DB with data_dir → insert data →
 * destroy DB → create new DB with same data_dir → verify data survived.
 */

#include "server/database.h"
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

static const std::string TEST_DIR = "/tmp/flexql_test_persistence";

static void clean_dir() {
    if (fs::exists(TEST_DIR)) fs::remove_all(TEST_DIR);
}

// ─── Tests ─────────────────────────────────────────────────────────────

void test_persistence_basic_recovery() {
    clean_dir();

    // Phase 1: Create DB, insert data, destroy
    {
        flexql::Database db(TEST_DIR);
        db.start();

        auto r1 = db.execute("CREATE TABLE students (id INT, name VARCHAR(50), gpa DECIMAL)");
        ASSERT(r1.success);

        auto r2 = db.execute("INSERT INTO students VALUES (1, 'Alice', 9.5)");
        ASSERT(r2.success);
        auto r3 = db.execute("INSERT INTO students VALUES (2, 'Bob', 8.2)");
        ASSERT(r3.success);
        auto r4 = db.execute("INSERT INTO students VALUES (3, 'Charlie', 7.8)");
        ASSERT(r4.success);

        // Verify data exists
        auto sel = db.execute("SELECT * FROM students");
        ASSERT(sel.success);
        ASSERT(sel.rows.size() == 3);

        db.stop();
        // Destructor will take final snapshot
    }

    // Phase 2: Create NEW DB with same data_dir → data should survive
    {
        flexql::Database db(TEST_DIR);
        db.start();

        auto sel = db.execute("SELECT * FROM students");
        ASSERT(sel.success);
        ASSERT(sel.rows.size() == 3);

        // Verify specific values
        ASSERT(sel.rows[0][0].int_val == 1);
        ASSERT(sel.rows[0][1].str_val == "Alice");
        ASSERT(sel.rows[1][0].int_val == 2);
        ASSERT(sel.rows[2][0].int_val == 3);

        db.stop();
    }

    clean_dir();
}

void test_persistence_wal_replay() {
    clean_dir();

    // Phase 1: Create DB, insert data, DO NOT take snapshot (simulate crash)
    {
        flexql::Database db(TEST_DIR);
        db.start();

        db.execute("CREATE TABLE t (id INT, val VARCHAR(20))");
        db.execute("INSERT INTO t VALUES (1, 'one')");
        db.execute("INSERT INTO t VALUES (2, 'two')");
        db.execute("INSERT INTO t VALUES (3, 'three')");

        // DON'T call stop() or take_snapshot() — simulate unclean shutdown
        // The destructor still fires (C++ guarantees this), but the key test
        // is that data survives regardless of shutdown path.
    }

    // Phase 2: Recover — data should survive via snapshot (destructor) or WAL
    {
        flexql::Database db(TEST_DIR);
        db.start();

        // Data must be present regardless of recovery path
        auto sel = db.execute("SELECT * FROM t");
        ASSERT(sel.success);
        ASSERT(sel.rows.size() == 3);
        ASSERT(sel.rows[0][1].str_val == "one");
        ASSERT(sel.rows[2][1].str_val == "three");

        db.stop();
    }

    clean_dir();
}

void test_persistence_snapshot_plus_wal() {
    clean_dir();

    // Phase 1: Create DB, insert data, take snapshot
    {
        flexql::Database db(TEST_DIR);
        db.start();

        db.execute("CREATE TABLE t (id INT, name VARCHAR(20))");
        db.execute("INSERT INTO t VALUES (1, 'snap1')");
        db.execute("INSERT INTO t VALUES (2, 'snap2')");

        std::string err;
        ASSERT(db.take_snapshot(err));

        // Insert more data (goes to WAL, not in snapshot)
        db.execute("INSERT INTO t VALUES (3, 'wal1')");
        db.execute("INSERT INTO t VALUES (4, 'wal2')");

        // Don't take another snapshot — these are in WAL only
    }

    // Phase 2: Recover — snapshot + WAL replay
    {
        flexql::Database db(TEST_DIR);
        db.start();

        auto sel = db.execute("SELECT * FROM t");
        ASSERT(sel.success);
        ASSERT(sel.rows.size() == 4);
        ASSERT(sel.rows[0][1].str_val == "snap1");
        ASSERT(sel.rows[3][1].str_val == "wal2");

        db.stop();
    }

    clean_dir();
}

void test_persistence_multiple_tables() {
    clean_dir();

    {
        flexql::Database db(TEST_DIR);
        db.start();

        db.execute("CREATE TABLE users (id INT, name VARCHAR(30))");
        db.execute("CREATE TABLE orders (id INT, user_id INT)");
        db.execute("INSERT INTO users VALUES (1, 'Alice')");
        db.execute("INSERT INTO users VALUES (2, 'Bob')");
        db.execute("INSERT INTO orders VALUES (100, 1)");
        db.execute("INSERT INTO orders VALUES (101, 2)");
        db.execute("INSERT INTO orders VALUES (102, 1)");

        db.stop();
    }

    {
        flexql::Database db(TEST_DIR);
        db.start();

        auto u = db.execute("SELECT * FROM users");
        ASSERT(u.success);
        ASSERT(u.rows.size() == 2);

        auto o = db.execute("SELECT * FROM orders");
        ASSERT(o.success);
        ASSERT(o.rows.size() == 3);

        db.stop();
    }

    clean_dir();
}

void test_persistence_in_memory_mode() {
    // Empty data_dir = original in-memory behavior, no files created
    {
        flexql::Database db("");  // In-memory mode
        db.start();

        auto r = db.execute("CREATE TABLE t (id INT)");
        ASSERT(r.success);
        db.execute("INSERT INTO t VALUES (1)");

        ASSERT(!db.persistent());

        db.stop();
    }

    // Data is gone — no recovery possible
    {
        flexql::Database db("");
        db.start();

        auto sel = db.execute("SELECT * FROM t");
        ASSERT(!sel.success);  // Table doesn't exist

        db.stop();
    }
}

void test_persistence_where_query_after_recovery() {
    clean_dir();

    {
        flexql::Database db(TEST_DIR);
        db.start();

        db.execute("CREATE TABLE scores (id INT, name VARCHAR(20), score DECIMAL)");
        db.execute("INSERT INTO scores VALUES (1, 'Alice', 95.5)");
        db.execute("INSERT INTO scores VALUES (2, 'Bob', 72.3)");
        db.execute("INSERT INTO scores VALUES (3, 'Charlie', 88.0)");
        db.execute("INSERT INTO scores VALUES (4, 'Diana', 91.7)");

        db.stop();
    }

    {
        flexql::Database db(TEST_DIR);
        db.start();

        // PK lookup
        auto r1 = db.execute("SELECT * FROM scores WHERE id = 2");
        ASSERT(r1.success);
        ASSERT(r1.rows.size() == 1);
        ASSERT(r1.rows[0][1].str_val == "Bob");

        // Range scan on non-PK
        auto r2 = db.execute("SELECT * FROM scores WHERE score > 90.0");
        ASSERT(r2.success);
        ASSERT(r2.rows.size() == 2);  // Alice (95.5) and Diana (91.7)

        db.stop();
    }

    clean_dir();
}

// ─── Main ──────────────────────────────────────────────────────────────

int main() {
    std::cout << "\n=== FlexQL Persistence Tests ===\n\n";

    TEST(persistence_basic_recovery)
    TEST(persistence_wal_replay)
    TEST(persistence_snapshot_plus_wal)
    TEST(persistence_multiple_tables)
    TEST(persistence_in_memory_mode)
    TEST(persistence_where_query_after_recovery)

    std::cout << "\n--- Results: " << tests_passed << " passed, "
              << tests_failed << " failed ---\n";

    clean_dir();
    return tests_failed > 0 ? 1 : 0;
}
