/*
 * lru_cache.cpp
 *
 * Standard LRU with a hash map + doubly linked list. O(1) everything.
 * Query normalization strips whitespace and uppercases so that
 * "select * from foo" and "SELECT *  FROM  FOO" hit the same entry.
 */

#include "lru_cache.h"
#include <algorithm>
#include <sstream>
#include <cctype>

namespace flexql {

LRUCache::LRUCache(size_t capacity) : capacity_(capacity) {}

std::string LRUCache::normalize(const std::string& query) {
    std::string result;
    result.reserve(query.size());
    bool in_space = false;

    for (char c : query) {
        if (std::isspace(c)) {
            if (!in_space && !result.empty()) {
                result += ' ';
                in_space = true;
            }
        } else {
            result += static_cast<char>(std::toupper(c));
            in_space = false;
        }
    }

    // Trim trailing space
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    // Remove trailing semicolons
    while (!result.empty() && result.back() == ';') {
        result.pop_back();
    }

    return result;
}

bool LRUCache::get(const std::string& query, CachedResult& result) {
    std::string key = normalize(query);

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = cache_map_.find(key);
    if (it == cache_map_.end()) {
        misses_++;
        return false;
    }

    // Move to front (most recently used)
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
    result = it->second->second;
    hits_++;
    return true;
}

void LRUCache::put(const std::string& query, const CachedResult& result) {
    std::string key = normalize(query);

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = cache_map_.find(key);
    if (it != cache_map_.end()) {
        // Update existing
        it->second->second = result;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    // Evict if at capacity
    if (cache_map_.size() >= capacity_) {
        auto& back = lru_list_.back();
        cache_map_.erase(back.first);
        lru_list_.pop_back();
    }

    // Insert new
    lru_list_.push_front({key, result});
    cache_map_[key] = lru_list_.begin();

    // Track table association (extract table name from normalized query)
    // Simple heuristic: look for "FROM <table>" pattern
    std::string upper = key;
    size_t from_pos = upper.find("FROM ");
    if (from_pos != std::string::npos) {
        size_t name_start = from_pos + 5;
        size_t name_end = upper.find_first_of(" ;,)", name_start);
        if (name_end == std::string::npos) name_end = upper.size();
        std::string table_name = upper.substr(name_start, name_end - name_start);
        table_entries_[table_name].push_back(key);
    }
}

void LRUCache::invalidate_table(const std::string& table_name) {
    std::string upper_table = table_name;
    std::transform(upper_table.begin(), upper_table.end(), upper_table.begin(), ::toupper);

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = table_entries_.find(upper_table);
    if (it == table_entries_.end()) return;

    for (const auto& key : it->second) {
        auto map_it = cache_map_.find(key);
        if (map_it != cache_map_.end()) {
            lru_list_.erase(map_it->second);
            cache_map_.erase(map_it);
        }
    }
    table_entries_.erase(it);
}

void LRUCache::clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    lru_list_.clear();
    cache_map_.clear();
    table_entries_.clear();
}

size_t LRUCache::size() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return cache_map_.size();
}

}  // namespace flexql
