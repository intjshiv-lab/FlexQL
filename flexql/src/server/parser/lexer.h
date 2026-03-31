/*
 * lexer.h — SQL tokenizer
 * Author: Ramesh Choudhary
 */

#ifndef FLEXQL_LEXER_H
#define FLEXQL_LEXER_H

#include <string>
#include <vector>

namespace flexql {

enum class TokenType {
    // Keywords
    KW_CREATE, KW_TABLE, KW_INSERT, KW_INTO, KW_VALUES,
    KW_SELECT, KW_FROM, KW_WHERE, KW_INNER, KW_JOIN, KW_ON,
    KW_INT, KW_DECIMAL, KW_VARCHAR, KW_DATETIME,

    // Literals
    IDENTIFIER,
    INT_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,

    // Operators
    OP_EQ,      // =
    OP_NE,      // != or <>
    OP_LT,      // <
    OP_GT,      // >
    OP_LE,      // <=
    OP_GE,      // >=

    // Punctuation
    LPAREN,     // (
    RPAREN,     // )
    COMMA,      // ,
    SEMICOLON,  // ;
    DOT,        // .
    STAR,       // *

    // End
    END_OF_INPUT,
    INVALID,
};

struct Token {
    TokenType   type;
    std::string value;
    int         line;
    int         col;

    Token() : type(TokenType::INVALID), line(0), col(0) {}
    Token(TokenType t, const std::string& v, int l, int c)
        : type(t), value(v), line(l), col(c) {}
};

class Lexer {
public:
    explicit Lexer(const std::string& input);

    // Tokenize the entire input. Returns list of tokens.
    std::vector<Token> tokenize();

    // Get the error message, if any.
    const std::string& error() const { return error_; }

private:
    char peek() const;
    char advance();
    void skip_whitespace();

    Token read_number();
    Token read_identifier_or_keyword();
    Token read_string();

    std::string input_;
    size_t      pos_;
    int         line_;
    int         col_;
    std::string error_;
};

}  // namespace flexql

#endif /* FLEXQL_LEXER_H */
