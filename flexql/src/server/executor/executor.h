/*
 * executor.h — query executor
 *
 * Takes an AST from the parser and runs it against the storage layer.
 * This is where all the pieces come together.
 *
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_EXECUTOR_H
#define FLEXQL_EXECUTOR_H

#include "common.h"
#include "server/parser/ast.h"
#include "server/storage/table.h"
#include "server/cache/lru_cache.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <functional>

namespace flexql {

class Executor {
public:
    // Result of a query execution
    struct QueryResult {
        bool                     success = false;
        std::string              error;
        std::vector<std::string> column_names;
        std::vector<Row>         rows;
        std::string              message;  // For non-SELECT (e.g., "Table created")
    };

    Executor();
    ~Executor() = default;

    // Execute a parsed statement
    QueryResult execute(const Statement& stmt, const std::string& raw_sql = "");

    // Direct SQL execution (parse + execute)
    QueryResult execute_sql(const std::string& sql);

    // Access cache stats
    LRUCache& cache() { return cache_; }

    // ─── Persistence support ───────────────────────────────────────────
    // Direct access to tables for snapshot save
    const std::unordered_map<std::string, std::unique_ptr<Table>>& tables() const {
        return tables_;
    }

    // Restore a table during recovery (snapshot load or WAL replay)
    void restore_table(const std::string& name, std::unique_ptr<Table> table) {
        std::unique_lock lock(tables_mutex_);
        tables_[name] = std::move(table);
    }

    // Number of tables
    size_t table_count() const {
        std::shared_lock lock(tables_mutex_);
        return tables_.size();
    }

private:
    QueryResult exec_create_table(const CreateTableStmt& stmt);
    QueryResult exec_insert(const InsertStmt& stmt);
    QueryResult exec_select(const SelectStmt& stmt, const std::string& raw_sql);

    // Convert AST comparison op to Table comparison op
    static Table::CmpOp convert_op(AstCmpOp op);

    // Parse a literal string into a Value of the given type
    static Value parse_literal(const std::string& literal, DataType type, std::string& error);

    // Table storage
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    mutable std::shared_mutex tables_mutex_;

    // Query cache
    LRUCache cache_;
};

}  // namespace flexql

#endif /* FLEXQL_EXECUTOR_H */
