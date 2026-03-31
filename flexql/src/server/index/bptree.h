/*
 * bptree.h — B+ tree index
 *
 * Order-256 with doubly-linked leaves so range scans don't have to
 * walk back up the tree. Binary search within each node.
 *
 * I went with B+ over red-black because the fan-out is way better for
 * cache lines. At order 256 we get ~256 keys per node which fits
 * nicely in a few cache lines, vs RB-tree's 2 keys per node.
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_BPTREE_H
#define FLEXQL_BPTREE_H

#include "common.h"
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

namespace flexql {

class BPTree {
public:
    static constexpr int ORDER = BPTREE_ORDER;       // Max keys per node
    static constexpr int MIN_KEYS = (ORDER - 1) / 2; // Min keys (except root)

    explicit BPTree(DataType key_type);
    ~BPTree();

    // Non-copyable
    BPTree(const BPTree&) = delete;
    BPTree& operator=(const BPTree&) = delete;

    // ─── Insert ────────────────────────────────────────────────────────
    // Insert a key → row_id mapping. Returns true on success.
    bool insert(const Value& key, uint64_t row_id);

    // ─── Point lookup ──────────────────────────────────────────────────
    // Find exact key. Returns row_id, or -1 if not found.
    int64_t find(const Value& key) const __attribute__((hot));

    // ─── Range scan ────────────────────────────────────────────────────
    // Calls `callback(row_id)` for each key satisfying the condition.
    // callback returns false to abort.
    using RangeCallback = std::function<bool(uint64_t row_id)>;

    void range_eq(const Value& key, RangeCallback cb) const __attribute__((hot));
    void range_lt(const Value& key, RangeCallback cb) const;
    void range_le(const Value& key, RangeCallback cb) const;
    void range_gt(const Value& key, RangeCallback cb) const;
    void range_ge(const Value& key, RangeCallback cb) const;
    void range_ne(const Value& key, RangeCallback cb) const;
    void scan_all(RangeCallback cb) const;

    // ─── Statistics ────────────────────────────────────────────────────
    uint64_t size()         const { return size_; }
    uint32_t height()       const { return height_; }
    size_t   memory_usage() const;

private:
    struct Node {
        bool              is_leaf;
        int               num_keys;
        Value             keys[ORDER];
        union {
            Node*         children[ORDER + 1];   // Internal node
            uint64_t      row_ids[ORDER];         // Leaf node
        };
        Node*             next;   // Leaf: next leaf
        Node*             prev;   // Leaf: prev leaf
        Node*             parent;

        Node(bool leaf) : is_leaf(leaf), num_keys(0), next(nullptr),
                          prev(nullptr), parent(nullptr) {
            if (leaf) {
                std::memset(row_ids, 0, sizeof(row_ids));
            } else {
                std::memset(children, 0, sizeof(children));
            }
        }
    };

    // Binary search: find index of first key >= target in node
    int lower_bound(const Node* node, const Value& key) const;

    // Find the leaf node where `key` should reside
    Node* find_leaf(const Value& key) const;

    // Split a full leaf node
    void split_leaf(Node* leaf);

    // Split a full internal node
    void split_internal(Node* node);

    // Insert into parent after a split
    void insert_into_parent(Node* left, const Value& key, Node* right);

    // Allocate a new node
    Node* alloc_node(bool is_leaf);

    // Free all nodes
    void free_tree(Node* node);

    DataType  key_type_;
    Node*     root_;
    Node*     leftmost_leaf_;   // For full scans
    uint64_t  size_;
    uint32_t  height_;

    // Node pool for contiguous allocation
    std::vector<std::unique_ptr<Node>> node_pool_;
};

}  // namespace flexql

#endif /* FLEXQL_BPTREE_H */
