/*
 * arena.cpp — arena allocator impl
 *
 * Pretty straightforward bump allocator. Keep the first chunk on reset()
 * so we don't thrash malloc for repeated load/clear cycles.
 */

#include "arena.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace flexql {

Arena::Arena(size_t chunk_size)
    : chunk_size_(chunk_size)
    , current_offset_(0)
    , total_allocated_(0)
{
    allocate_new_chunk(chunk_size_);
}

Arena::~Arena() {
    for (auto& chunk : chunks_) {
        std::free(chunk.data);
    }
}

void Arena::allocate_new_chunk(size_t min_size) {
    size_t alloc_size = std::max(min_size, chunk_size_);

    uint8_t* data = static_cast<uint8_t*>(std::malloc(alloc_size));
    if (!data) {
        throw std::bad_alloc();
    }

    chunks_.push_back({data, alloc_size});
    current_offset_ = 0;
}

void* Arena::allocate(size_t size, size_t alignment) {
    if (size == 0) return nullptr;

    // align up to the requested boundary (usually 8)
    Chunk& current = chunks_.back();
    size_t aligned_offset = (current_offset_ + alignment - 1) & ~(alignment - 1);

    if (aligned_offset + size > current.capacity) {
        // current chunk is full, grab a new one
        allocate_new_chunk(size + alignment);
        aligned_offset = 0;
    }

    Chunk& chunk = chunks_.back();
    void* ptr = chunk.data + aligned_offset;
    current_offset_ = aligned_offset + size;
    total_allocated_ += size;

    return ptr;
}

void Arena::reset() {
    // Keep the first chunk, free the rest
    for (size_t i = 1; i < chunks_.size(); i++) {
        std::free(chunks_[i].data);
    }
    if (!chunks_.empty()) {
        chunks_.resize(1);
    }
    current_offset_ = 0;
    total_allocated_ = 0;
}

}  // namespace flexql
