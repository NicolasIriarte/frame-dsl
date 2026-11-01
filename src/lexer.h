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
  /** @brief `:=` fixes a field's value and `?=` gives it a default (#144). */
  TOK_COLON_EQUALS,
  TOK_QUESTION_EQUALS,
  /** @brief `==` and `!=`, which only a condition compares with, `[ @if: valof(x) == 1; ]` (#12). */
  TOK_EQUAL_EQUAL,
  TOK_NOT_EQUAL,
  /** @brief The rest of a condition's operators (#126): orderings, `&&`, `||`, and `&` for a mask. */
  TOK_LESS,
  TOK_LESS_EQUAL,
  TOK_GREATER,
  TOK_GREATER_EQUAL,
  TOK_AND_AND,
  TOK_OR_OR,
  TOK_AMPERSAND,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,
  /** @brief In a dotted name: an override's path, `inner.id = 3;`, or an imported one, `ns.Header`. */
  TOK_DOT,
  /** @brief `"..."`, an import's path. `text`/`length` are the contents, without the quotes. */
  TOK_STRING,

  /** @brief Produced instead of a token when the input cannot be tokenised.
   *
   * The parser stops at it rather than trying to make sense of what follows.
   */
  TOK_INVALID,

  /** @brief `# ...` to the end of the line. Only from lexer_next_spelled(); a load never sees one. */
  TOK_COMMENT,
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

/** @brief Whether `token`, which may be NULL, gives a field its value: `:=` or `?=` (#144). */
static inline bool token_is_value_operator(const Token *token) {
  return token != NULL
         && (token->kind == TOK_EQUALS || token->kind == TOK_COLON_EQUALS || token->kind == TOK_QUESTION_EQUALS);
}

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
 * @brief As lexer_next(), for the formatter: comments come back as TOK_COMMENT tokens instead of being
 * skipped, and every token -- not only a name -- has `text`/`length` set to its exact spelling in the
 * source, so `0xCAFE` can be written back as `0xCAFE` and a string keeps its quotes.
 */
Token lexer_next_spelled(Lexer *lexer);

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
