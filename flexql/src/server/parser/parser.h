/*
 * parser.h — recursive descent SQL parser
 */

#ifndef FLEXQL_PARSER_H
#define FLEXQL_PARSER_H

#include "ast.h"
#include "lexer.h"
#include <memory>
#include <string>

namespace flexql {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    // Parse tokens into a Statement. Returns nullptr on error.
    std::unique_ptr<Statement> parse();

    const std::string& error() const { return error_; }

private:
    const Token& current() const;
    const Token& peek_next() const;
    bool match(TokenType type);
    bool expect(TokenType type, const std::string& context);
    void advance();

    std::unique_ptr<Statement> parse_create_table();
    std::unique_ptr<Statement> parse_insert();
    std::unique_ptr<Statement> parse_select();

    AstCmpOp parse_cmp_op();
    ColumnRef parse_column_ref();
    WhereClause parse_where();

    std::vector<Token> tokens_;
    size_t             pos_;
    std::string        error_;
};

// Convenience: parse a SQL string into a Statement
std::unique_ptr<Statement> parse_sql(const std::string& sql, std::string& error);

}  // namespace flexql

#endif /* FLEXQL_PARSER_H */
