#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"

typedef enum {
  TOK_EOF,
  TOK_IDENT,
  TOK_INT,
  /** @brief A decimal with a fraction or an exponent, `0.0625` or `4e-3`. Calibrations only. */
  TOK_REAL,

  /* Keywords. Reserved: none of these can be an identifier. The setting names are deliberately not
   * here -- they live behind '@' and are ordinary identifiers the parser looks up in a table, so
   * nothing stops a field being called `bit_order`. */
  TOK_PKT,
  TOK_VALOF,
  TOK_BLOCK,

  TOK_AT,
  TOK_COLON,
  TOK_SEMICOLON,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_LBRACKET,
  TOK_RBRACKET,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_EQUALS,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  /** @brief Only in an override's path, `inner.id = 3;`. */
  TOK_DOT,

  /** @brief Produced instead of a token when the input cannot be tokenised.
   *
   * The parser stops at it rather than trying to make sense of what follows.
   */
  TOK_INVALID,
} TokenKind;

typedef struct {
  TokenKind kind;
  int line;
  int column;
  /** @brief TOK_IDENT: points into the source, not terminated. */
  const char *text;
  size_t length;
  /** @brief TOK_INT. */
  uint64_t value;
  /** @brief TOK_REAL, and TOK_INT as well, so a calibration can take either without asking. */
  double real;
} Token;

typedef struct {
  const char *source;
  size_t position;
  int line;
  int column;
  DiagList *diags;
} Lexer;

void lexer_init(Lexer *lexer, const char *source, DiagList *diags);

Token lexer_next(Lexer *lexer);

/**
 * @brief For diagnostics: "expected X, found <this>".
 * @param token The token to describe.
 * @return The description. Static storage, valid until the next call.
 */
const char *token_describe(const Token *token);

/**
 * @brief The spelling of a kind, for "expected" halves of a message.
 * @param kind The kind to spell.
 * @return The spelling.
 */
const char *token_kind_name(TokenKind kind);
