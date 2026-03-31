/*
 * lru_cache.h — LRU query result cache
 *
 * Caches SELECT results keyed by normalized query string.
 * Invalidated on any write to the relevant table.
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_LRU_CACHE_H
#define FLEXQL_LRU_CACHE_H

#include "common.h"
#include <unordered_map>
#include <list>
#include <string>
#include <vector>
#include <shared_mutex>
#include <functional>

namespace flexql {

class LRUCache {
public:
    // A cached query result: column names + rows
    struct CachedResult {
        std::vector<std::string> column_names;
        std::vector<Row>         rows;
    };

    explicit LRUCache(size_t capacity = LRU_CACHE_CAPACITY);

    // Lookup a query. Returns true and fills `result` on cache hit.
    bool get(const std::string& query, CachedResult& result) __attribute__((hot));

    // Insert a query result into the cache.
    void put(const std::string& query, const CachedResult& result);

    // Invalidate all entries for a given table name.
    void invalidate_table(const std::string& table_name);

    // Clear entire cache.
    void clear();

    // Statistics
    uint64_t hits()   const { return hits_; }
    uint64_t misses() const { return misses_; }
    size_t   size()   const;

private:
    // Normalize a query string for cache key (lowercase keywords, trim whitespace)
    static std::string normalize(const std::string& query);

    // Hash function for cache key
    static size_t hash_query(const std::string& normalized);

    size_t capacity_;

    // LRU list: front = most recently used
    using LRUList = std::list<std::pair<std::string, CachedResult>>;
    LRUList lru_list_;

    // Map from query hash to iterator in the LRU list
    std::unordered_map<std::string, LRUList::iterator> cache_map_;

    // Track which tables each cache entry touches
    std::unordered_map<std::string, std::vector<std::string>> table_entries_;

    mutable std::shared_mutex mutex_;
    uint64_t hits_   = 0;
    uint64_t misses_ = 0;
};

}  // namespace flexql

#endif /* FLEXQL_LRU_CACHE_H */
