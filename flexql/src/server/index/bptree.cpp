/*
 * bptree.cpp
 *
 * The insert logic is the trickiest part of this whole project honestly.
 * Split-and-promote when a leaf overflows, then propagate up. Had to
 * debug this for a while before range scans worked correctly.
 *
 * TODO: implement delete properly (currently just marks, doesn't merge nodes)
 */

#include "bptree.h"
#include <algorithm>
#include <cassert>

namespace flexql {

BPTree::BPTree(DataType key_type)
    : key_type_(key_type)
    , root_(nullptr)
    , leftmost_leaf_(nullptr)
    , size_(0)
    , height_(0)
{
    root_ = alloc_node(true);
    leftmost_leaf_ = root_;
    height_ = 1;
}

BPTree::~BPTree() {
    // node_pool_ unique_ptrs handle cleanup
}

BPTree::Node* BPTree::alloc_node(bool is_leaf) {
    node_pool_.push_back(std::make_unique<Node>(is_leaf));
    return node_pool_.back().get();
}

// Binary search: find index of first key >= target
int BPTree::lower_bound(const Node* node, const Value& key) const {
    int lo = 0, hi = node->num_keys;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (node->keys[mid] < key) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

BPTree::Node* BPTree::find_leaf(const Value& key) const {
    Node* cur = root_;
    while (cur && !cur->is_leaf) {
        int idx = lower_bound(cur, key);
        // If key >= all keys, go to last child
        if (idx == cur->num_keys) {
            cur = cur->children[idx];
        } else if (cur->keys[idx] == key) {
            // Equal: go right child (keys[idx] route → children[idx+1])
            cur = cur->children[idx + 1];
        } else {
            cur = cur->children[idx];
        }
    }
    return cur;
}

// ─── Insert ────────────────────────────────────────────────────────────────

bool BPTree::insert(const Value& key, uint64_t row_id) {
    Node* leaf = find_leaf(key);
    if (!leaf) return false;

    // Find insertion position using binary search
    int pos = lower_bound(leaf, key);

    // Check for duplicate (overwrite for PK — debatable but keeps it simple)
    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        leaf->row_ids[pos] = row_id; // Update existing
        return true;
    }

    // Shift right to make room
    for (int i = leaf->num_keys; i > pos; i--) {
        leaf->keys[i]    = leaf->keys[i - 1];
        leaf->row_ids[i] = leaf->row_ids[i - 1];
    }
    leaf->keys[pos]    = key;
    leaf->row_ids[pos] = row_id;
    leaf->num_keys++;
    size_++;

    // Split if overflow
    if (leaf->num_keys >= ORDER) {
        split_leaf(leaf);
    }

    return true;
}

void BPTree::split_leaf(Node* leaf) {
    Node* new_leaf = alloc_node(true);
    int mid = leaf->num_keys / 2;

    // Move upper half to new_leaf
    new_leaf->num_keys = leaf->num_keys - mid;
    for (int i = 0; i < new_leaf->num_keys; i++) {
        new_leaf->keys[i]    = leaf->keys[mid + i];
        new_leaf->row_ids[i] = leaf->row_ids[mid + i];
    }
    leaf->num_keys = mid;

    // Fix linked list
    new_leaf->next = leaf->next;
    new_leaf->prev = leaf;
    if (leaf->next) leaf->next->prev = new_leaf;
    leaf->next = new_leaf;

    // Promote the first key of new_leaf to parent
    insert_into_parent(leaf, new_leaf->keys[0], new_leaf);
}

void BPTree::split_internal(Node* node) {
    Node* new_node = alloc_node(false);
    int mid = node->num_keys / 2;

    Value promote_key = node->keys[mid];

    // Move keys and children after mid to new_node
    new_node->num_keys = node->num_keys - mid - 1;
    for (int i = 0; i < new_node->num_keys; i++) {
        new_node->keys[i] = node->keys[mid + 1 + i];
    }
    for (int i = 0; i <= new_node->num_keys; i++) {
        new_node->children[i] = node->children[mid + 1 + i];
        if (new_node->children[i]) {
            new_node->children[i]->parent = new_node;
        }
    }
    node->num_keys = mid;

    insert_into_parent(node, promote_key, new_node);
}

void BPTree::insert_into_parent(Node* left, const Value& key, Node* right) {
    if (left == root_) {
        // Create new root
        Node* new_root = alloc_node(false);
        new_root->keys[0]     = key;
        new_root->children[0] = left;
        new_root->children[1] = right;
        new_root->num_keys    = 1;
        left->parent  = new_root;
        right->parent = new_root;
        root_ = new_root;
        height_++;
        return;
    }

    Node* parent = left->parent;
    right->parent = parent;

    // Find position of left in parent's children
    int pos = 0;
    while (pos <= parent->num_keys && parent->children[pos] != left) {
        pos++;
    }

    // Insert key and right child after pos
    for (int i = parent->num_keys; i > pos; i--) {
        parent->keys[i]         = parent->keys[i - 1];
        parent->children[i + 1] = parent->children[i];
    }
    parent->keys[pos]         = key;
    parent->children[pos + 1] = right;
    parent->num_keys++;

    if (parent->num_keys >= ORDER) {
        split_internal(parent);
    }
}

// ─── Point Lookup ──────────────────────────────────────────────────────────

int64_t BPTree::find(const Value& key) const {
    Node* leaf = find_leaf(key);
    if (!leaf) return -1;

    int pos = lower_bound(leaf, key);
    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        return static_cast<int64_t>(leaf->row_ids[pos]);
    }
    return -1;
}

// ─── Range Scans ───────────────────────────────────────────────────────────

void BPTree::range_eq(const Value& key, RangeCallback cb) const {
    Node* leaf = find_leaf(key);
    if (!leaf) return;
    int pos = lower_bound(leaf, key);
    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        cb(leaf->row_ids[pos]);
    }
}

void BPTree::range_gt(const Value& key, RangeCallback cb) const {
    Node* leaf = find_leaf(key);
    if (!leaf) return;

    int pos = lower_bound(leaf, key);
    // Skip equal keys
    while (leaf) {
        for (int i = pos; i < leaf->num_keys; i++) {
            if (leaf->keys[i] > key) {
                if (!cb(leaf->row_ids[i])) return;
            }
        }
        leaf = leaf->next;
        pos = 0;
    }
}

void BPTree::range_ge(const Value& key, RangeCallback cb) const {
    Node* leaf = find_leaf(key);
    if (!leaf) return;

    int pos = lower_bound(leaf, key);
    while (leaf) {
        for (int i = pos; i < leaf->num_keys; i++) {
            if (!cb(leaf->row_ids[i])) return;
        }
        leaf = leaf->next;
        pos = 0;
    }
}

void BPTree::range_lt(const Value& key, RangeCallback cb) const {
    // Start from leftmost leaf, go until we reach key
    Node* leaf = leftmost_leaf_;
    while (leaf) {
        for (int i = 0; i < leaf->num_keys; i++) {
            if (leaf->keys[i] >= key) return;
            if (!cb(leaf->row_ids[i])) return;
        }
        leaf = leaf->next;
    }
}

void BPTree::range_le(const Value& key, RangeCallback cb) const {
    Node* leaf = leftmost_leaf_;
    while (leaf) {
        for (int i = 0; i < leaf->num_keys; i++) {
            if (leaf->keys[i] > key) return;
            if (!cb(leaf->row_ids[i])) return;
        }
        leaf = leaf->next;
    }
}

void BPTree::range_ne(const Value& key, RangeCallback cb) const {
    Node* leaf = leftmost_leaf_;
    while (leaf) {
        for (int i = 0; i < leaf->num_keys; i++) {
            if (!(leaf->keys[i] == key)) {
                if (!cb(leaf->row_ids[i])) return;
            }
        }
        leaf = leaf->next;
    }
}

void BPTree::scan_all(RangeCallback cb) const {
    Node* leaf = leftmost_leaf_;
    while (leaf) {
        for (int i = 0; i < leaf->num_keys; i++) {
            if (!cb(leaf->row_ids[i])) return;
        }
        leaf = leaf->next;
    }
}

// ─── Memory Usage ──────────────────────────────────────────────────────────

size_t BPTree::memory_usage() const {
    return node_pool_.size() * sizeof(Node);
}

}  // namespace flexql
