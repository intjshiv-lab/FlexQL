/*
 * ============================================================================
 *  FlexQL — B+ Tree Index Tests
 * ============================================================================
 */

#include "server/index/bptree.h"
#include "common.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include <random>

using namespace flexql;

// Utility: collect row_ids from a callback-based range scan into a vector
static std::vector<uint64_t> collect(BPTree& tree,
    void (BPTree::*fn)(const Value&, BPTree::RangeCallback) const,
    const Value& key) {
    std::vector<uint64_t> out;
    (tree.*fn)(key, [&](uint64_t rid) -> bool { out.push_back(rid); return true; });
    return out;
}

void test_insert_and_find() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 10; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i - 1));
    }

    for (int i = 1; i <= 10; ++i) {
        int64_t rid = tree.find(Value::make_int(i));
        assert(rid == static_cast<int64_t>(i - 1));
    }

    // Not found
    int64_t nf = tree.find(Value::make_int(99));
    assert(nf == -1);

    std::cout << "[PASS] test_insert_and_find\n";
}

void test_range_eq() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 100; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_eq, Value::make_int(50));
    assert(results.size() == 1);
    assert(results[0] == 50);
    std::cout << "[PASS] test_range_eq\n";
}

void test_range_lt() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 10; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_lt, Value::make_int(5));
    assert(results.size() == 4);  // 1, 2, 3, 4
    std::cout << "[PASS] test_range_lt\n";
}

void test_range_le() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 10; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_le, Value::make_int(5));
    assert(results.size() == 5);  // 1, 2, 3, 4, 5
    std::cout << "[PASS] test_range_le\n";
}

void test_range_gt() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 10; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_gt, Value::make_int(7));
    assert(results.size() == 3);  // 8, 9, 10
    std::cout << "[PASS] test_range_gt\n";
}

void test_range_ge() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 10; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_ge, Value::make_int(7));
    assert(results.size() == 4);  // 7, 8, 9, 10
    std::cout << "[PASS] test_range_ge\n";
}

void test_range_ne() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 5; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    auto results = collect(tree, &BPTree::range_ne, Value::make_int(3));
    assert(results.size() == 4);  // 1, 2, 4, 5
    std::cout << "[PASS] test_range_ne\n";
}

void test_scan_all() {
    BPTree tree(DataType::INT);
    for (int i = 1; i <= 50; ++i) {
        tree.insert(Value::make_int(i), static_cast<uint64_t>(i));
    }

    std::vector<uint64_t> all;
    tree.scan_all([&](uint64_t rid) -> bool { all.push_back(rid); return true; });
    assert(all.size() == 50);
    // Should be in sorted order
    for (size_t i = 1; i < all.size(); ++i) {
        assert(all[i] > all[i - 1]);
    }
    std::cout << "[PASS] test_scan_all\n";
}

void test_large_insert() {
    BPTree tree(DataType::INT);
    // Insert 10,000 keys in random order
    std::vector<int> keys(10000);
    std::iota(keys.begin(), keys.end(), 1);
    std::mt19937 rng(42);
    std::shuffle(keys.begin(), keys.end(), rng);

    for (int k : keys) {
        tree.insert(Value::make_int(k), static_cast<uint64_t>(k));
    }

    // Verify all can be found
    for (int k : keys) {
        int64_t rid = tree.find(Value::make_int(k));
        assert(rid == static_cast<int64_t>(k));
    }

    // Verify sorted scan
    std::vector<uint64_t> all;
    tree.scan_all([&](uint64_t rid) -> bool { all.push_back(rid); return true; });
    assert(all.size() == 10000);
    for (size_t i = 0; i < all.size(); ++i) {
        assert(all[i] == i + 1);
    }

    std::cout << "[PASS] test_large_insert (10,000 keys)\n";
}

void test_duplicate_keys() {
    BPTree tree(DataType::INT);
    tree.insert(Value::make_int(5), 100);
    tree.insert(Value::make_int(5), 200);
    tree.insert(Value::make_int(5), 300);

    int64_t rid = tree.find(Value::make_int(5));
    assert(rid >= 0);  // At least one found
    std::cout << "[PASS] test_duplicate_keys\n";
}

void test_decimal_keys() {
    BPTree tree(DataType::DECIMAL);
    for (int i = 1; i <= 20; ++i) {
        tree.insert(Value::make_decimal(i * 0.5), static_cast<uint64_t>(i));
    }

    int64_t rid = tree.find(Value::make_decimal(5.0));
    assert(rid >= 0);

    auto lt_results = collect(tree, &BPTree::range_lt, Value::make_decimal(5.0));
    assert(lt_results.size() == 9);  // 0.5, 1.0, ..., 4.5
    std::cout << "[PASS] test_decimal_keys\n";
}

void test_varchar_keys() {
    BPTree tree(DataType::VARCHAR);
    tree.insert(Value::make_varchar("apple"), 1);
    tree.insert(Value::make_varchar("banana"), 2);
    tree.insert(Value::make_varchar("cherry"), 3);
    tree.insert(Value::make_varchar("date"), 4);

    int64_t rid = tree.find(Value::make_varchar("banana"));
    assert(rid == 2);

    auto gt = collect(tree, &BPTree::range_gt, Value::make_varchar("banana"));
    assert(gt.size() == 2);  // cherry, date
    std::cout << "[PASS] test_varchar_keys\n";
}

int main() {
    std::cout << "=== B+ Tree Index Tests ===\n";
    test_insert_and_find();
    test_range_eq();
    test_range_lt();
    test_range_le();
    test_range_gt();
    test_range_ge();
    test_range_ne();
    test_scan_all();
    test_large_insert();
    test_duplicate_keys();
    test_decimal_keys();
    test_varchar_keys();
    std::cout << "=== All B+ Tree tests passed! ===\n";
    return 0;
}
