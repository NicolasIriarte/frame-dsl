#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"

typedef enum {
  TOK_EOF,
  TOK_IDENT,
  TOK_INT,

  /* Keywords. Reserved: none of these can be an identifier. */
  TOK_PKT,
  TOK_VALOF,
  TOK_MAX,
  TOK_BIT_ORDER,
  TOK_MSB_FIRST,
  TOK_LSB_FIRST,
  TOK_BYTE_ORDER,
  TOK_BIG_ENDIAN,
  TOK_LITTLE_ENDIAN,

  TOK_COLON,
  TOK_SEMICOLON,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_EQUALS,
  TOK_PLUS,
  TOK_MINUS,
  TOK_STAR,
  TOK_SLASH,

  /* Produced instead of a token when the input cannot be tokenised. The parser stops at it rather
   * than trying to make sense of what follows. */
  TOK_INVALID,
} TokenKind;

typedef struct {
  TokenKind kind;
  int line;
  int column;
  /* TOK_IDENT: points into the source, not terminated. */
  const char *text;
  size_t length;
  /* TOK_INT. */
  uint64_t value;
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

/* For diagnostics: "expected X, found <this>". Static storage, valid until the next call. */
const char *token_describe(const Token *token);

/* The spelling of a kind, for "expected" halves of a message. */
const char *token_kind_name(TokenKind kind);
