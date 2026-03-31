/*
 * arena.h — slab-based memory pool for row storage
 *
 * The whole point of this is to avoid calling malloc/free for every single
 * row. At 10M rows that would be brutal. Instead we grab big chunks upfront
 * and bump-allocate within them.
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_ARENA_H
#define FLEXQL_ARENA_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <mutex>

namespace flexql {

class Arena {
public:
    explicit Arena(size_t chunk_size = 1 << 20 /* 1 MB */);
    ~Arena();

    // Non-copyable, non-movable
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // Allocate `size` bytes aligned to `alignment`.
    // Returns nullptr only on catastrophic failure.
    void* allocate(size_t size, size_t alignment = 8);

    // Reset: reclaim ALL memory (invalidates all pointers).
    void reset();

    // Statistics
    size_t total_allocated() const { return total_allocated_; }
    size_t total_capacity()  const { return chunks_.size() * chunk_size_; }
    size_t num_chunks()      const { return chunks_.size(); }

private:
    struct Chunk {
        uint8_t* data;
        size_t   capacity;
    };

    void allocate_new_chunk(size_t min_size);

    size_t chunk_size_;
    size_t current_offset_;     // Offset within current chunk
    size_t total_allocated_;

    std::vector<Chunk> chunks_;
};

}  // namespace flexql

#endif /* FLEXQL_ARENA_H */
