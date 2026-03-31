/*
 * executor.cpp
 *
 * This was the hardest file to get right. Had to rewrite it twice because
 * the first version had type mismatches all over the place between the AST
 * nodes and the storage layer APIs. Lesson learned: define your interfaces
 * before writing the glue code.
 *
 * FIXME: JOIN currently builds a full hash map in memory. Fine for the
 * benchmark but won't scale for truly huge tables. Would need to spill
 * to disk or do a sort-merge join instead.
 */

#include "server/executor/executor.h"
#include "server/parser/parser.h"
#include "server/storage/schema.h"
#include <algorithm>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <unordered_set>
#include <unordered_map>

namespace flexql {

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

static std::string to_upper(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

Table::CmpOp Executor::convert_op(AstCmpOp op) {
    switch (op) {
        case AstCmpOp::EQ: return Table::CmpOp::EQ;
        case AstCmpOp::NE: return Table::CmpOp::NE;
        case AstCmpOp::LT: return Table::CmpOp::LT;
        case AstCmpOp::LE: return Table::CmpOp::LE;
        case AstCmpOp::GT: return Table::CmpOp::GT;
        case AstCmpOp::GE: return Table::CmpOp::GE;
    }
    return Table::CmpOp::EQ;
}

Value Executor::parse_literal(const std::string& literal, DataType type, std::string& error) {
    error.clear();
    switch (type) {
        case DataType::INT: {
            try {
                int32_t v = static_cast<int32_t>(std::stoi(literal));
                return Value::make_int(v);
            } catch (...) {
                error = "Invalid INT literal: " + literal;
                return Value();
            }
        }
        case DataType::DECIMAL: {
            try {
                double v = std::stod(literal);
                return Value::make_decimal(v);
            } catch (...) {
                error = "Invalid DECIMAL literal: " + literal;
                return Value();
            }
        }
        case DataType::VARCHAR: {
            return Value::make_varchar(literal);
        }
        case DataType::DATETIME: {
            try {
                int64_t v = std::stoll(literal);
                return Value::make_datetime(v);
            } catch (...) {
                error = "Invalid DATETIME literal: " + literal;
                return Value();
            }
        }
    }
    error = "Unknown data type";
    return Value();
}

// ---------------------------------------------------------------------------
//  Constructor
// ---------------------------------------------------------------------------

Executor::Executor() : cache_(LRU_CACHE_CAPACITY) {}

// ---------------------------------------------------------------------------
//  Public Interface
// ---------------------------------------------------------------------------

Executor::QueryResult Executor::execute(const Statement& stmt, const std::string& raw_sql) {
    switch (stmt.type) {
        case StmtType::CREATE_TABLE:
            return exec_create_table(*stmt.create_table);
        case StmtType::INSERT:
            return exec_insert(*stmt.insert);
        case StmtType::SELECT:
            return exec_select(*stmt.select, raw_sql);
    }
    QueryResult r;
    r.success = false;
    r.error = "Unknown statement type";
    return r;
}

Executor::QueryResult Executor::execute_sql(const std::string& sql) {
    std::string err;
    auto stmt = parse_sql(sql, err);
    if (!stmt) {
        QueryResult r;
        r.success = false;
        r.error = "Parse error: " + err;
        return r;
    }
    return execute(*stmt, sql);
}

// ---------------------------------------------------------------------------
//  CREATE TABLE
// ---------------------------------------------------------------------------

Executor::QueryResult Executor::exec_create_table(const CreateTableStmt& stmt) {
    QueryResult result;
    std::string table_name = to_upper(stmt.table_name);

    std::unique_lock lock(tables_mutex_);

    if (tables_.count(table_name)) {
        result.success = false;
        result.error = "Table already exists: " + table_name;
        return result;
    }

    // Build schema
    Schema schema;
    for (const auto& col : stmt.columns) {
        DataType dt;
        std::string type_upper = to_upper(col.type_name);
        if (type_upper == "INT")           dt = DataType::INT;
        else if (type_upper == "DECIMAL")  dt = DataType::DECIMAL;
        else if (type_upper == "DATETIME") dt = DataType::DATETIME;
        else if (type_upper.substr(0, 7) == "VARCHAR") dt = DataType::VARCHAR;
        else {
            result.success = false;
            result.error = "Unknown data type: " + col.type_name;
            return result;
        }

        uint16_t max_len = DEFAULT_VARCHAR_MAX;
        if (dt == DataType::VARCHAR && col.varchar_len > 0) {
            max_len = col.varchar_len;
        }

        schema.add_column(to_upper(col.name), dt, max_len);
    }

    auto table = std::make_unique<Table>(table_name, schema);
    tables_[table_name] = std::move(table);

    result.success = true;
    result.message = "Table " + table_name + " created";
    return result;
}

// ---------------------------------------------------------------------------
//  INSERT
// ---------------------------------------------------------------------------

Executor::QueryResult Executor::exec_insert(const InsertStmt& stmt) {
    QueryResult result;
    std::string table_name = to_upper(stmt.table_name);

    std::shared_lock lock(tables_mutex_);

    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        result.success = false;
        result.error = "Table not found: " + table_name;
        return result;
    }

    Table* table = it->second.get();
    const Schema& schema = table->schema();

    int rows_inserted = 0;

    for (const auto& row_vals : stmt.batch_values) {
        if (row_vals.size() != schema.columns().size()) {
            result.success = false;
            result.error = "Column count mismatch: expected " +
                           std::to_string(schema.columns().size()) +
                           ", got " + std::to_string(row_vals.size());
            return result;
        }

        Row row;
        row.reserve(row_vals.size());
        for (size_t i = 0; i < row_vals.size(); ++i) {
            std::string err;
            Value val = parse_literal(row_vals[i], schema.columns()[i].type, err);
            if (!err.empty()) {
                result.success = false;
                result.error = err;
                return result;
            }
            row.push_back(std::move(val));
        }

        std::string err;
        if (!table->insert(row, err)) {
            result.success = false;
            result.error = err;
            return result;
        }
        rows_inserted++;
    }

    // Invalidate cached queries for this table
    cache_.invalidate_table(table_name);

    result.success = true;
    result.message = std::to_string(rows_inserted) + " rows inserted";
    return result;
}

// ---------------------------------------------------------------------------
//  SELECT (with optional WHERE and INNER JOIN)
// ---------------------------------------------------------------------------

Executor::QueryResult Executor::exec_select(const SelectStmt& stmt, const std::string& raw_sql) {
    QueryResult result;

    // --- Check cache first ---
    if (!raw_sql.empty()) {
        LRUCache::CachedResult cached;
        if (cache_.get(raw_sql, cached)) {
            result.success = true;
            result.column_names = cached.column_names;
            result.rows = cached.rows;
            return result;
        }
    }

    std::string table_name = to_upper(stmt.table_name);
    std::shared_lock lock(tables_mutex_);

    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        result.success = false;
        result.error = "Table not found: " + table_name;
        return result;
    }

    Table* table = it->second.get();
    const Schema& schema = table->schema();

    // ---- Handle INNER JOIN ----
    if (stmt.has_join) {
        std::string right_table_name = to_upper(stmt.join.right_table);
        auto jt = tables_.find(right_table_name);
        if (jt == tables_.end()) {
            result.success = false;
            result.error = "Join table not found: " + right_table_name;
            return result;
        }

        Table* right_table = jt->second.get();
        const Schema& right_schema = right_table->schema();

        // Resolve ON columns
        std::string left_on_col = to_upper(stmt.join.left_col.column);
        std::string right_on_col = to_upper(stmt.join.right_col.column);

        int left_on_idx = schema.find_column(left_on_col);
        int right_on_idx = right_schema.find_column(right_on_col);

        if (left_on_idx < 0) {
            result.success = false;
            result.error = "Unknown column in JOIN ON (left): " + left_on_col;
            return result;
        }
        if (right_on_idx < 0) {
            result.success = false;
            result.error = "Unknown column in JOIN ON (right): " + right_on_col;
            return result;
        }

        auto left_rows = table->scan_all();
        auto right_rows = right_table->scan_all();

        // Resolve output columns
        std::vector<std::string> out_col_names;
        std::vector<std::pair<int, int>> col_sources;  // {0=left/1=right, col_idx}

        bool join_select_all = stmt.select_all
                               || stmt.columns.empty()
                               || (stmt.columns.size() == 1 && stmt.columns[0].column == "*");

        if (join_select_all) {
            for (size_t c = 0; c < schema.columns().size(); ++c) {
                out_col_names.push_back(table_name + "." + schema.columns()[c].name);
                col_sources.push_back({0, static_cast<int>(c)});
            }
            for (size_t c = 0; c < right_schema.columns().size(); ++c) {
                out_col_names.push_back(right_table_name + "." + right_schema.columns()[c].name);
                col_sources.push_back({1, static_cast<int>(c)});
            }
        } else {
            for (const auto& cr : stmt.columns) {
                std::string tbl = to_upper(cr.table);
                std::string col = to_upper(cr.column);

                if (tbl == table_name || tbl.empty()) {
                    int idx = schema.find_column(col);
                    if (idx >= 0) {
                        out_col_names.push_back(tbl.empty() ? col : tbl + "." + col);
                        col_sources.push_back({0, idx});
                        continue;
                    }
                }
                if (tbl == right_table_name || tbl.empty()) {
                    int idx = right_schema.find_column(col);
                    if (idx >= 0) {
                        out_col_names.push_back(tbl.empty() ? col : tbl + "." + col);
                        col_sources.push_back({1, idx});
                        continue;
                    }
                }
                result.success = false;
                result.error = "Unknown column in SELECT: " + (tbl.empty() ? col : tbl + "." + col);
                return result;
            }
        }

        std::vector<Row> joined_rows;
        joined_rows.reserve(std::max(left_rows.size(), right_rows.size()));

        AstCmpOp join_op = stmt.join.op;

        if (join_op == AstCmpOp::EQ) {
            // Zero-alloc hash join: use Value directly as key (no to_string conversion)
            std::unordered_map<Value, std::vector<size_t>> hash_index;
            hash_index.reserve(right_rows.size());
            for (size_t i = 0; i < right_rows.size(); ++i) {
                hash_index[right_rows[i][right_on_idx]].push_back(i);
            }

            for (const auto& left_row : left_rows) {
                auto hit = hash_index.find(left_row[left_on_idx]);
                if (hit == hash_index.end()) continue;

                for (size_t ri : hit->second) {
                    const auto& right_row = right_rows[ri];

                    // Apply WHERE filter if present
                    if (stmt.has_where) {
                        std::string wc = to_upper(stmt.where.column.column);
                        std::string wt = to_upper(stmt.where.column.table);
                        const Row* check_row = nullptr;
                        const Schema* check_schema = nullptr;

                        if (wt == table_name || wt.empty()) {
                            if (schema.find_column(wc) >= 0) {
                                check_row = &left_row;
                                check_schema = &schema;
                            }
                        }
                        if (!check_row && (wt == right_table_name || wt.empty())) {
                            if (right_schema.find_column(wc) >= 0) {
                                check_row = &right_row;
                                check_schema = &right_schema;
                            }
                        }
                        if (check_row && check_schema) {
                            int wi = check_schema->find_column(wc);
                            std::string err;
                            Value wv = parse_literal(stmt.where.literal,
                                                     check_schema->columns()[wi].type, err);
                            if (err.empty()) {
                                const Value& actual = (*check_row)[wi];
                                bool pass = false;
                                switch (stmt.where.op) {
                                    case AstCmpOp::EQ: pass = (actual == wv); break;
                                    case AstCmpOp::NE: pass = (actual != wv); break;
                                    case AstCmpOp::LT: pass = (actual < wv);  break;
                                    case AstCmpOp::LE: pass = (actual <= wv); break;
                                    case AstCmpOp::GT: pass = (actual > wv);  break;
                                    case AstCmpOp::GE: pass = (actual >= wv); break;
                                }
                                if (!pass) continue;
                            }
                        }
                    }

                    // Build output row
                    Row out;
                    out.reserve(col_sources.size());
                    for (const auto& [side, idx] : col_sources) {
                        if (side == 0) out.push_back(left_row[idx]);
                        else           out.push_back(right_row[idx]);
                    }
                    joined_rows.push_back(std::move(out));
                }
            }
        } else {
            // Nested loop join for non-EQ
            for (const auto& left_row : left_rows) {
                const Value& left_val = left_row[left_on_idx];
                
                for (const auto& right_row : right_rows) {
                    const Value& right_val = right_row[right_on_idx];
                    
                    bool join_pass = false;
                    switch (join_op) {
                        case AstCmpOp::EQ: join_pass = (left_val == right_val); break;
                        case AstCmpOp::NE: join_pass = (left_val != right_val); break;
                        case AstCmpOp::LT: join_pass = (left_val < right_val); break;
                        case AstCmpOp::LE: join_pass = (left_val <= right_val); break;
                        case AstCmpOp::GT: join_pass = (left_val > right_val); break;
                        case AstCmpOp::GE: join_pass = (left_val >= right_val); break;
                    }
                    
                    if (!join_pass) continue;
                    
                    // Apply WHERE filter if present
                    if (stmt.has_where) {
                        std::string wc = to_upper(stmt.where.column.column);
                        std::string wt = to_upper(stmt.where.column.table);
                        const Row* check_row = nullptr;
                        const Schema* check_schema = nullptr;

                        if (wt == table_name || wt.empty()) {
                            if (schema.find_column(wc) >= 0) {
                                check_row = &left_row;
                                check_schema = &schema;
                            }
                        }
                        if (!check_row && (wt == right_table_name || wt.empty())) {
                            if (right_schema.find_column(wc) >= 0) {
                                check_row = &right_row;
                                check_schema = &right_schema;
                            }
                        }
                        if (check_row && check_schema) {
                            int wi = check_schema->find_column(wc);
                            std::string err;
                            Value wv = parse_literal(stmt.where.literal,
                                                     check_schema->columns()[wi].type, err);
                            if (err.empty()) {
                                const Value& actual = (*check_row)[wi];
                                bool pass = false;
                                switch (stmt.where.op) {
                                    case AstCmpOp::EQ: pass = (actual == wv); break;
                                    case AstCmpOp::NE: pass = (actual != wv); break;
                                    case AstCmpOp::LT: pass = (actual < wv);  break;
                                    case AstCmpOp::LE: pass = (actual <= wv); break;
                                    case AstCmpOp::GT: pass = (actual > wv);  break;
                                    case AstCmpOp::GE: pass = (actual >= wv); break;
                                }
                                if (!pass) continue;
                            }
                        }
                    }

                    // Build output row
                    Row out;
                    out.reserve(col_sources.size());
                    for (const auto& [side, idx] : col_sources) {
                        if (side == 0) out.push_back(left_row[idx]);
                        else           out.push_back(right_row[idx]);
                    }
                    joined_rows.push_back(std::move(out));
                }
            }
        }

        result.success = true;
        result.column_names = std::move(out_col_names);
        result.rows = std::move(joined_rows);

        // Cache
        if (!raw_sql.empty()) {
            LRUCache::CachedResult cr;
            cr.column_names = result.column_names;
            cr.rows = result.rows;
            cache_.put(raw_sql, cr);
        }

        return result;
    }

    // --- Resolve column list (non-JOIN) ---
    std::vector<std::string> select_cols;
    bool is_select_all = stmt.select_all 
                         || stmt.columns.empty()
                         || (stmt.columns.size() == 1 && stmt.columns[0].column == "*");
    
    if (is_select_all) {
        for (const auto& col : schema.columns()) {
            select_cols.push_back(col.name);
        }
    } else {
        for (const auto& cr : stmt.columns) {
            select_cols.push_back(to_upper(cr.column));
        }
    }

    // Resolve projection indices
    std::vector<int> proj_indices;
    bool need_projection = false;
    for (const auto& col_name : select_cols) {
        int idx = schema.find_column(col_name);
        if (idx < 0) {
            result.success = false;
            result.error = "Unknown column: " + col_name;
            return result;
        }
        proj_indices.push_back(idx);
    }
    if (proj_indices.size() != schema.columns().size()) {
        need_projection = true;
    } else {
        for (size_t i = 0; i < proj_indices.size(); ++i) {
            if (proj_indices[i] != static_cast<int>(i)) { need_projection = true; break; }
        }
    }

    // --- Execute scan ---
    std::vector<Row> rows;

    if (stmt.has_where) {
        std::string col_name = to_upper(stmt.where.column.column);
        int col_idx = schema.find_column(col_name);
        if (col_idx < 0) {
            result.success = false;
            result.error = "Unknown column in WHERE: " + col_name;
            return result;
        }

        std::string err;
        Value where_val = parse_literal(stmt.where.literal,
                                         schema.columns()[col_idx].type, err);
        if (!err.empty()) {
            result.success = false;
            result.error = err;
            return result;
        }

        Table::CmpOp op = convert_op(stmt.where.op);

        // Use index for primary key queries, otherwise full scan with filter
        if (col_idx == static_cast<int>(schema.pk_index())) {
            rows = table->index_range(op, where_val);
        } else {
            rows = table->scan_where(col_idx, op, where_val);
        }
    } else {
        rows = table->scan_all();
    }

    // --- Apply projection ---
    if (need_projection) {
        std::vector<Row> projected;
        projected.reserve(rows.size());
        for (const auto& row : rows) {
            Row proj_row;
            proj_row.reserve(proj_indices.size());
            for (int idx : proj_indices) {
                proj_row.push_back(row[idx]);
            }
            projected.push_back(std::move(proj_row));
        }
        rows = std::move(projected);
    }

    result.success = true;
    result.column_names = select_cols;
    result.rows = std::move(rows);

    // --- Cache the result ---
    if (!raw_sql.empty()) {
        LRUCache::CachedResult cr;
        cr.column_names = result.column_names;
        cr.rows = result.rows;
        cache_.put(raw_sql, cr);
    }

    return result;
}

}  // namespace flexql
