/*
 * ============================================================================
 *  FlexQL — Arena Allocator Tests
 * ============================================================================
 */

#include "server/storage/arena.h"
#include <cassert>
#include <iostream>
#include <cstring>

using namespace flexql;

void test_basic_alloc() {
    Arena arena;
    void* p = arena.allocate(64);
    assert(p != nullptr);
    std::memset(p, 0xAB, 64);
    std::cout << "[PASS] test_basic_alloc\n";
}

void test_alignment() {
    Arena arena;
    void* p1 = arena.allocate(1);
    void* p2 = arena.allocate(8, 8);
    assert(reinterpret_cast<uintptr_t>(p2) % 8 == 0);

    void* p3 = arena.allocate(16, 16);
    assert(reinterpret_cast<uintptr_t>(p3) % 16 == 0);
    std::cout << "[PASS] test_alignment\n";
}

void test_large_alloc() {
    Arena arena(1024);  // Small chunks
    // Allocate more than one chunk
    void* p1 = arena.allocate(512);
    void* p2 = arena.allocate(512);
    void* p3 = arena.allocate(512);
    assert(p1 != nullptr);
    assert(p2 != nullptr);
    assert(p3 != nullptr);
    assert(arena.num_chunks() >= 2);
    std::cout << "[PASS] test_large_alloc\n";
}

void test_reset() {
    Arena arena(1024);
    arena.allocate(512);
    arena.allocate(512);
    size_t before_chunks = arena.num_chunks();
    arena.reset();
    assert(arena.total_allocated() == 0);
    std::cout << "[PASS] test_reset\n";
}

void test_stats() {
    Arena arena(4096);
    arena.allocate(100);
    arena.allocate(200);
    assert(arena.total_allocated() >= 300);
    assert(arena.total_capacity() >= 4096);
    std::cout << "[PASS] test_stats\n";
}

void test_many_small_allocs() {
    Arena arena;
    for (int i = 0; i < 10000; ++i) {
        void* p = arena.allocate(16);
        assert(p != nullptr);
    }
    std::cout << "[PASS] test_many_small_allocs\n";
}

int main() {
    std::cout << "=== Arena Allocator Tests ===\n";
    test_basic_alloc();
    test_alignment();
    test_large_alloc();
    test_reset();
    test_stats();
    test_many_small_allocs();
    std::cout << "All arena tests passed!\n";
    return 0;
}
