/*
 * ============================================================================
 *  FlexQL — Parser (Lexer + Parser) Tests
 * ============================================================================
 */

#include "server/parser/parser.h"
#include "server/parser/lexer.h"
#include <cassert>
#include <iostream>

using namespace flexql;

void test_lexer_create() {
    Lexer lex("CREATE TABLE users (id INT, name VARCHAR(50))");
    auto tokens = lex.tokenize();

    assert(tokens.size() >= 10);
    assert(tokens[0].type == TokenType::KW_CREATE);
    assert(tokens[1].type == TokenType::KW_TABLE);
    assert(tokens[2].type == TokenType::IDENTIFIER);
    assert(tokens[2].value == "users");
    std::cout << "[PASS] test_lexer_create\n";
}

void test_lexer_select() {
    Lexer lex("SELECT id, name FROM users WHERE score >= 90.5");
    auto tokens = lex.tokenize();

    assert(tokens[0].type == TokenType::KW_SELECT);
    // Check for >= operator
    bool found_ge = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::OP_GE) found_ge = true;
    }
    assert(found_ge);
    std::cout << "[PASS] test_lexer_select\n";
}

void test_lexer_string_literal() {
    Lexer lex("INSERT INTO t VALUES ('hello world')");
    auto tokens = lex.tokenize();

    bool found_str = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::STRING_LITERAL && t.value == "hello world")
            found_str = true;
    }
    assert(found_str);
    std::cout << "[PASS] test_lexer_string_literal\n";
}

void test_lexer_negative_number() {
    Lexer lex("INSERT INTO t VALUES (-42, -3.14)");
    auto tokens = lex.tokenize();

    bool found_neg_int = false;
    bool found_neg_dec = false;
    for (const auto& t : tokens) {
        if (t.type == TokenType::INT_LITERAL && t.value == "-42") found_neg_int = true;
        if (t.type == TokenType::FLOAT_LITERAL && t.value == "-3.14") found_neg_dec = true;
    }
    assert(found_neg_int);
    assert(found_neg_dec);
    std::cout << "[PASS] test_lexer_negative_number\n";
}

void test_parse_create_table() {
    std::string err;
    auto stmt = parse_sql("CREATE TABLE employees (id INT, name VARCHAR(100), salary DECIMAL, hired DATETIME)", err);
    assert(stmt != nullptr);
    assert(stmt->type == StmtType::CREATE_TABLE);
    assert(stmt->create_table->table_name == "employees");
    assert(stmt->create_table->columns.size() == 4);
    assert(stmt->create_table->columns[0].name == "id");
    assert(stmt->create_table->columns[0].type == "INT");
    assert(stmt->create_table->columns[1].varchar_max == 100);
    assert(stmt->create_table->columns[3].type == "DATETIME");
    std::cout << "[PASS] test_parse_create_table\n";
}

void test_parse_insert() {
    std::string err;
    auto stmt = parse_sql("INSERT INTO employees VALUES (1, 'Alice', 75000.50, 1700000000)", err);
    assert(stmt != nullptr);
    assert(stmt->type == StmtType::INSERT);
    assert(stmt->insert->table_name == "employees");
    assert(stmt->insert->values.size() == 4);
    assert(stmt->insert->values[0] == "1");
    assert(stmt->insert->values[1] == "Alice");
    assert(stmt->insert->values[2] == "75000.50");
    std::cout << "[PASS] test_parse_insert\n";
}

void test_parse_select_star() {
    std::string err;
    auto stmt = parse_sql("SELECT * FROM employees", err);
    assert(stmt != nullptr);
    assert(stmt->type == StmtType::SELECT);
    assert(stmt->select->table_name == "employees");
    assert(stmt->select->columns.size() == 1);
    assert(stmt->select->columns[0].column == "*");
    assert(!stmt->select->where);
    assert(!stmt->select->join);
    std::cout << "[PASS] test_parse_select_star\n";
}

void test_parse_select_columns() {
    std::string err;
    auto stmt = parse_sql("SELECT id, name FROM employees", err);
    assert(stmt != nullptr);
    assert(stmt->select->columns.size() == 2);
    assert(stmt->select->columns[0].column == "id");
    assert(stmt->select->columns[1].column == "name");
    std::cout << "[PASS] test_parse_select_columns\n";
}

void test_parse_select_where() {
    std::string err;
    auto stmt = parse_sql("SELECT * FROM employees WHERE salary > 50000", err);
    assert(stmt != nullptr);
    assert(stmt->select->where.has_value());
    assert(stmt->select->where->column.column == "salary");
    assert(stmt->select->where->op == AstCmpOp::GT);
    assert(stmt->select->where->literal == "50000");
    std::cout << "[PASS] test_parse_select_where\n";
}

void test_parse_select_where_string() {
    std::string err;
    auto stmt = parse_sql("SELECT * FROM employees WHERE name = 'Alice'", err);
    assert(stmt != nullptr);
    assert(stmt->select->where->literal == "Alice");
    assert(stmt->select->where->op == AstCmpOp::EQ);
    std::cout << "[PASS] test_parse_select_where_string\n";
}

void test_parse_inner_join() {
    std::string err;
    auto stmt = parse_sql(
        "SELECT e.name, d.dept_name FROM employees e INNER JOIN departments d ON e.dept_id = d.id",
        err);
    assert(stmt != nullptr);
    assert(stmt->select->join.has_value());
    assert(stmt->select->join->table_name == "d");
    assert(stmt->select->join->on_left.column == "dept_id");
    assert(stmt->select->join->on_right.column == "id");
    std::cout << "[PASS] test_parse_inner_join\n";
}

void test_parse_error() {
    std::string err;
    auto stmt = parse_sql("INVALID QUERY BLAH", err);
    assert(stmt == nullptr);
    assert(!err.empty());
    std::cout << "[PASS] test_parse_error (error: " << err << ")\n";
}

int main() {
    std::cout << "=== Parser Tests ===\n";
    test_lexer_create();
    test_lexer_select();
    test_lexer_string_literal();
    test_lexer_negative_number();
    test_parse_create_table();
    test_parse_insert();
    test_parse_select_star();
    test_parse_select_columns();
    test_parse_select_where();
    test_parse_select_where_string();
    test_parse_inner_join();
    test_parse_error();
    std::cout << "All parser tests passed!\n";
    return 0;
}
