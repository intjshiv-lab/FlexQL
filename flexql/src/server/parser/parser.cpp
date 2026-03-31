/*
 * parser.cpp — recursive descent SQL parser
 *
 * No yacc/bison here. The grammar is small enough (~15 productions) that
 * doing it by hand gives us better error messages and zero dependencies.
 * Same approach SQLite uses, which is reassuring.
 */

#include "parser.h"
#include <algorithm>

namespace flexql {

Parser::Parser(const std::vector<Token> &tokens) : tokens_(tokens), pos_(0) {}

const Token &Parser::current() const {
  static Token eof(TokenType::END_OF_INPUT, "", 0, 0);
  if (pos_ >= tokens_.size())
    return eof;
  return tokens_[pos_];
}

const Token &Parser::peek_next() const {
  static Token eof(TokenType::END_OF_INPUT, "", 0, 0);
  if (pos_ + 1 >= tokens_.size())
    return eof;
  return tokens_[pos_ + 1];
}

bool Parser::match(TokenType type) {
  if (current().type == type) {
    advance();
    return true;
  }
  return false;
}

bool Parser::expect(TokenType type, const std::string &context) {
  if (!match(type)) {
    error_ = "Expected " + context + " at line " +
             std::to_string(current().line) + " col " +
             std::to_string(current().col) + ", got '" + current().value + "'";
    return false;
  }
  return true;
}

void Parser::advance() {
  if (pos_ < tokens_.size())
    pos_++;
}

// ─── Top-level parse ──────────────────────────────────────────────────────

std::unique_ptr<Statement> Parser::parse() {
  if (current().type == TokenType::KW_CREATE) {
    return parse_create_table();
  } else if (current().type == TokenType::KW_INSERT) {
    return parse_insert();
  } else if (current().type == TokenType::KW_SELECT) {
    return parse_select();
  }

  error_ = "Unexpected token '" + current().value + "' at line " +
           std::to_string(current().line) +
           ". Expected CREATE, INSERT, or SELECT.";
  return nullptr;
}

// ─── CREATE TABLE name (col1 TYPE, col2 TYPE, ...) ────────────────────────

std::unique_ptr<Statement> Parser::parse_create_table() {
  advance(); // consume CREATE
  if (!expect(TokenType::KW_TABLE, "TABLE"))
    return nullptr;

  auto stmt = std::make_unique<CreateTableStmt>();
  if (current().type != TokenType::IDENTIFIER) {
    error_ = "Expected table name after CREATE TABLE";
    return nullptr;
  }
  stmt->table_name = current().value;
  advance();

  if (!expect(TokenType::LPAREN, "'('"))
    return nullptr;

  // Parse columns
  while (current().type != TokenType::RPAREN &&
         current().type != TokenType::END_OF_INPUT) {
    AstColumnDef col;

    if (current().type != TokenType::IDENTIFIER) {
      error_ = "Expected column name, got '" + current().value + "'";
      return nullptr;
    }
    col.name = current().value;
    advance();

    // Type
    switch (current().type) {
    case TokenType::KW_INT:
      col.type_name = "INT";
      break;
    case TokenType::KW_DECIMAL:
      col.type_name = "DECIMAL";
      break;
    case TokenType::KW_VARCHAR:
      col.type_name = "VARCHAR";
      break;
    case TokenType::KW_DATETIME:
      col.type_name = "DATETIME";
      break;
    default:
      error_ = "Expected type for column '" + col.name + "', got '" +
               current().value + "'";
      return nullptr;
    }
    advance();

    // Optional VARCHAR(N)
    if (col.type_name == "VARCHAR" && current().type == TokenType::LPAREN) {
      advance(); // consume (
      if (current().type != TokenType::INT_LITERAL) {
        error_ = "Expected VARCHAR length";
        return nullptr;
      }
      col.varchar_len = static_cast<uint16_t>(std::stoi(current().value));
      advance();
      if (!expect(TokenType::RPAREN, "')'"))
        return nullptr;
    }

    stmt->columns.push_back(col);

    if (!match(TokenType::COMMA))
      break;
  }

  if (!expect(TokenType::RPAREN, "')'"))
    return nullptr;
  match(TokenType::SEMICOLON); // optional

  auto result = std::make_unique<Statement>();
  result->type = StmtType::CREATE_TABLE;
  result->create_table = std::move(stmt);
  return result;
}

// ─── INSERT INTO table VALUES (v1, v2, ...) ──────────────────────────────

std::unique_ptr<Statement> Parser::parse_insert() {
  advance(); // consume INSERT
  if (!expect(TokenType::KW_INTO, "INTO"))
    return nullptr;

  auto stmt = std::make_unique<InsertStmt>();
  if (current().type != TokenType::IDENTIFIER) {
    error_ = "Expected table name after INSERT INTO";
    return nullptr;
  }
  stmt->table_name = current().value;
  advance();

  if (!expect(TokenType::KW_VALUES, "VALUES"))
    return nullptr;

  while (true) {
    if (!expect(TokenType::LPAREN, "'('"))
      return nullptr;

    std::vector<std::string> row_values;
    // Parse values
    while (current().type != TokenType::RPAREN &&
           current().type != TokenType::END_OF_INPUT) {
      switch (current().type) {
      case TokenType::INT_LITERAL:
      case TokenType::FLOAT_LITERAL:
      case TokenType::STRING_LITERAL:
        row_values.push_back(current().value);
        break;
      default:
        row_values.push_back(current().value);
        break;
      }
      advance();
      if (!match(TokenType::COMMA))
        break;
    }

    if (!expect(TokenType::RPAREN, "')'"))
      return nullptr;
    stmt->batch_values.push_back(std::move(row_values));

    if (!match(TokenType::COMMA))
      break;
  }

  match(TokenType::SEMICOLON);

  auto result = std::make_unique<Statement>();
  result->type = StmtType::INSERT;
  result->insert = std::move(stmt);
  return result;
}

// ─── SELECT ... FROM ... [WHERE ...] [INNER JOIN ...] ────────────────────

ColumnRef Parser::parse_column_ref() {
  ColumnRef ref;
  ref.column = current().value;
  advance();

  // Check for table.column
  if (current().type == TokenType::DOT) {
    advance(); // consume dot
    ref.table = ref.column;
    ref.column = current().value;
    advance();
  }

  return ref;
}

AstCmpOp Parser::parse_cmp_op() {
  switch (current().type) {
  case TokenType::OP_EQ:
    advance();
    return AstCmpOp::EQ;
  case TokenType::OP_NE:
    advance();
    return AstCmpOp::NE;
  case TokenType::OP_LT:
    advance();
    return AstCmpOp::LT;
  case TokenType::OP_GT:
    advance();
    return AstCmpOp::GT;
  case TokenType::OP_LE:
    advance();
    return AstCmpOp::LE;
  case TokenType::OP_GE:
    advance();
    return AstCmpOp::GE;
  default:
    error_ = "Expected comparison operator, got '" + current().value + "'";
    return AstCmpOp::EQ;
  }
}

WhereClause Parser::parse_where() {
  WhereClause w;
  w.column = parse_column_ref();
  w.op = parse_cmp_op();

  // The literal value
  w.literal = current().value;
  advance();
  return w;
}

std::unique_ptr<Statement> Parser::parse_select() {
  advance(); // consume SELECT

  auto stmt = std::make_unique<SelectStmt>();

  // Parse column list or *
  if (current().type == TokenType::STAR) {
    stmt->select_all = true;
    advance();
  } else {
    while (true) {
      if (current().type != TokenType::IDENTIFIER) {
        error_ = "Expected column name or *, got '" + current().value + "'";
        return nullptr;
      }
      stmt->columns.push_back(parse_column_ref());
      if (!match(TokenType::COMMA))
        break;
    }
  }

  // FROM
  if (!expect(TokenType::KW_FROM, "FROM"))
    return nullptr;

  if (current().type != TokenType::IDENTIFIER) {
    error_ = "Expected table name after FROM";
    return nullptr;
  }
  stmt->table_name = current().value;
  advance();

  // Optional INNER JOIN
  if (current().type == TokenType::KW_INNER) {
    advance(); // consume INNER
    if (!expect(TokenType::KW_JOIN, "JOIN"))
      return nullptr;

    stmt->has_join = true;
    if (current().type != TokenType::IDENTIFIER) {
      error_ = "Expected table name after INNER JOIN";
      return nullptr;
    }
    stmt->join.right_table = current().value;
    advance();

    if (!expect(TokenType::KW_ON, "ON"))
      return nullptr;

    // Parse join condition: t1.col = t2.col
    stmt->join.left_col = parse_column_ref();

    if (current().type != TokenType::OP_EQ) {
      error_ = "Expected '=' in JOIN condition";
      return nullptr;
    }
    advance();

    stmt->join.right_col = parse_column_ref();
  }

  // Optional WHERE
  if (current().type == TokenType::KW_WHERE) {
    advance(); // consume WHERE
    stmt->has_where = true;
    stmt->where = parse_where();
  }

  match(TokenType::SEMICOLON);

  auto result = std::make_unique<Statement>();
  result->type = StmtType::SELECT;
  result->select = std::move(stmt);
  return result;
}

// ─── Convenience function ─────────────────────────────────────────────────

std::unique_ptr<Statement> parse_sql(const std::string &sql,
                                     std::string &error) {
  Lexer lexer(sql);
  auto tokens = lexer.tokenize();

  if (!lexer.error().empty()) {
    error = lexer.error();
    return nullptr;
  }

  Parser parser(tokens);
  auto stmt = parser.parse();

  if (!stmt) {
    error = parser.error();
  }
  return stmt;
}

} // namespace flexql
