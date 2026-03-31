/*
 * ast.h — Abstract Syntax Tree node definitions
 *
 * These are the data structures the parser spits out. The executor
 * then walks them to figure out what to actually do.
 */

#ifndef FLEXQL_AST_H
#define FLEXQL_AST_H

#include "common.h"
#include <string>
#include <vector>
#include <memory>

namespace flexql {

/* ─── Statement Types ──────────────────────────────────────────────────── */
enum class StmtType {
    CREATE_TABLE,
    INSERT,
    SELECT,
};

/* ─── Comparison Operators ─────────────────────────────────────────────── */
enum class AstCmpOp {
    EQ,  // =
    NE,  // !=
    LT,  // <
    GT,  // >
    LE,  // <=
    GE,  // >=
};

inline const char* cmpop_to_string(AstCmpOp op) {
    switch (op) {
        case AstCmpOp::EQ: return "=";
        case AstCmpOp::NE: return "!=";
        case AstCmpOp::LT: return "<";
        case AstCmpOp::GT: return ">";
        case AstCmpOp::LE: return "<=";
        case AstCmpOp::GE: return ">=";
    }
    return "?";
}

/* ─── Column Reference ─────────────────────────────────────────────────── */
struct ColumnRef {
    std::string table;   // optional table prefix (for JOINs)
    std::string column;

    std::string full_name() const {
        return table.empty() ? column : table + "." + column;
    }
};

/* ─── WHERE clause ─────────────────────────────────────────────────────── */
struct WhereClause {
    ColumnRef   column;
    AstCmpOp    op;
    std::string literal;     // Raw literal value as string
};

/* ─── JOIN clause ──────────────────────────────────────────────────────── */
struct JoinClause {
    std::string right_table;
    ColumnRef   left_col;
    ColumnRef   right_col;
    AstCmpOp    op = AstCmpOp::EQ;
};

/* ─── Column Definition (for CREATE TABLE) ─────────────────────────────── */
struct AstColumnDef {
    std::string name;
    std::string type_name;  // "INT", "DECIMAL", "VARCHAR", "DATETIME"
    uint16_t    varchar_len = DEFAULT_VARCHAR_MAX;
};

/* ─── CREATE TABLE Statement ───────────────────────────────────────────── */
struct CreateTableStmt {
    std::string                table_name;
    std::vector<AstColumnDef>  columns;
};

/* ─── INSERT Statement ─────────────────────────────────────────────────── */
struct InsertStmt {
    std::string                             table_name;
    std::vector<std::vector<std::string>>   batch_values;     // Multiple rows of raw literal values
};

/* ─── SELECT Statement ─────────────────────────────────────────────────── */
struct SelectStmt {
    std::string              table_name;
    std::vector<ColumnRef>   columns;    // empty = SELECT *
    bool                     select_all = false;

    // Optional WHERE
    bool                     has_where = false;
    WhereClause              where;

    // Optional INNER JOIN
    bool                     has_join = false;
    JoinClause               join;
};

/* ─── Top-level parsed statement ───────────────────────────────────────── */
struct Statement {
    StmtType type;
    std::unique_ptr<CreateTableStmt> create_table;
    std::unique_ptr<InsertStmt>      insert;
    std::unique_ptr<SelectStmt>      select;
};

}  // namespace flexql

#endif /* FLEXQL_AST_H */
