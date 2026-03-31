/*
 * lexer.cpp — SQL tokenizer
 *
 * Turns raw SQL text into a token stream. Nothing fancy — just walks
 * the input char-by-char and matches keywords, literals, operators.
 */

#include "lexer.h"
#include <cctype>
#include <algorithm>
#include <unordered_map>

namespace flexql {

static const std::unordered_map<std::string, TokenType> KEYWORDS = {
    {"CREATE",   TokenType::KW_CREATE},
    {"TABLE",    TokenType::KW_TABLE},
    {"INSERT",   TokenType::KW_INSERT},
    {"INTO",     TokenType::KW_INTO},
    {"VALUES",   TokenType::KW_VALUES},
    {"SELECT",   TokenType::KW_SELECT},
    {"FROM",     TokenType::KW_FROM},
    {"WHERE",    TokenType::KW_WHERE},
    {"INNER",    TokenType::KW_INNER},
    {"JOIN",     TokenType::KW_JOIN},
    {"ON",       TokenType::KW_ON},
    {"INT",      TokenType::KW_INT},
    {"DECIMAL",  TokenType::KW_DECIMAL},
    {"VARCHAR",  TokenType::KW_VARCHAR},
    {"DATETIME", TokenType::KW_DATETIME},
};

Lexer::Lexer(const std::string& input)
    : input_(input), pos_(0), line_(1), col_(1) {}

char Lexer::peek() const {
    if (pos_ >= input_.size()) return '\0';
    return input_[pos_];
}

char Lexer::advance() {
    char c = input_[pos_++];
    if (c == '\n') { line_++; col_ = 1; }
    else { col_++; }
    return c;
}

void Lexer::skip_whitespace() {
    while (pos_ < input_.size() && std::isspace(input_[pos_])) {
        advance();
    }
}

Token Lexer::read_number() {
    int start_col = col_;
    std::string num;
    bool has_dot = false;

    while (pos_ < input_.size() && (std::isdigit(peek()) || peek() == '.')) {
        if (peek() == '.') {
            if (has_dot) break;  // Second dot — stop
            has_dot = true;
        }
        num += advance();
    }

    TokenType type = has_dot ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL;
    return Token(type, num, line_, start_col);
}

Token Lexer::read_identifier_or_keyword() {
    int start_col = col_;
    std::string ident;

    while (pos_ < input_.size() && (std::isalnum(peek()) || peek() == '_')) {
        ident += advance();
    }

    // Convert to uppercase for keyword matching
    std::string upper = ident;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    auto it = KEYWORDS.find(upper);
    if (it != KEYWORDS.end()) {
        return Token(it->second, upper, line_, start_col);
    }

    return Token(TokenType::IDENTIFIER, ident, line_, start_col);
}

Token Lexer::read_string() {
    int start_col = col_;
    char quote = advance(); // consume opening quote
    std::string str;

    while (pos_ < input_.size() && peek() != quote) {
        if (peek() == '\\') {
            advance(); // skip escape
            if (pos_ < input_.size()) str += advance();
        } else {
            str += advance();
        }
    }

    if (pos_ < input_.size()) {
        advance(); // consume closing quote
    } else {
        error_ = "Unterminated string literal";
    }

    return Token(TokenType::STRING_LITERAL, str, line_, start_col);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (pos_ < input_.size()) {
        skip_whitespace();
        if (pos_ >= input_.size()) break;

        char c = peek();
        int start_col = col_;

        if (std::isdigit(c) || (c == '-' && pos_ + 1 < input_.size() && std::isdigit(input_[pos_ + 1]))) {
            // Negative numbers
            if (c == '-') {
                std::string num;
                num += advance();
                Token t = read_number();
                t.value = num + t.value;
                t.col = start_col;
                // If the number has a dot, it's a float
                if (t.value.find('.') != std::string::npos) {
                    t.type = TokenType::FLOAT_LITERAL;
                }
                tokens.push_back(t);
            } else {
                tokens.push_back(read_number());
            }
        } else if (std::isalpha(c) || c == '_') {
            tokens.push_back(read_identifier_or_keyword());
        } else if (c == '\'' || c == '"') {
            tokens.push_back(read_string());
        } else {
            advance();
            switch (c) {
                case '(':  tokens.emplace_back(TokenType::LPAREN,    "(", line_, start_col); break;
                case ')':  tokens.emplace_back(TokenType::RPAREN,    ")", line_, start_col); break;
                case ',':  tokens.emplace_back(TokenType::COMMA,     ",", line_, start_col); break;
                case ';':  tokens.emplace_back(TokenType::SEMICOLON, ";", line_, start_col); break;
                case '.':  tokens.emplace_back(TokenType::DOT,       ".", line_, start_col); break;
                case '*':  tokens.emplace_back(TokenType::STAR,      "*", line_, start_col); break;
                case '=':  tokens.emplace_back(TokenType::OP_EQ,     "=", line_, start_col); break;
                case '<':
                    if (peek() == '=') {
                        advance();
                        tokens.emplace_back(TokenType::OP_LE, "<=", line_, start_col);
                    } else if (peek() == '>') {
                        advance();
                        tokens.emplace_back(TokenType::OP_NE, "<>", line_, start_col);
                    } else {
                        tokens.emplace_back(TokenType::OP_LT, "<", line_, start_col);
                    }
                    break;
                case '>':
                    if (peek() == '=') {
                        advance();
                        tokens.emplace_back(TokenType::OP_GE, ">=", line_, start_col);
                    } else {
                        tokens.emplace_back(TokenType::OP_GT, ">", line_, start_col);
                    }
                    break;
                case '!':
                    if (peek() == '=') {
                        advance();
                        tokens.emplace_back(TokenType::OP_NE, "!=", line_, start_col);
                    } else {
                        error_ = "Unexpected character '!' at line " +
                                 std::to_string(line_) + " col " + std::to_string(start_col);
                        tokens.emplace_back(TokenType::INVALID, "!", line_, start_col);
                    }
                    break;
                default:
                    error_ = "Unexpected character '" + std::string(1, c) +
                             "' at line " + std::to_string(line_) +
                             " col " + std::to_string(start_col);
                    tokens.emplace_back(TokenType::INVALID, std::string(1, c), line_, start_col);
                    break;
            }
        }
    }

    tokens.emplace_back(TokenType::END_OF_INPUT, "", line_, col_);
    return tokens;
}

}  // namespace flexql
