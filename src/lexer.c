#include "lexer.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *spelling;
  TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
    {"pkt", TOK_PKT},
    {"valof", TOK_VALOF},
    {"max", TOK_MAX},
    {"bit_order", TOK_BIT_ORDER},
    {"msb_first", TOK_MSB_FIRST},
    {"lsb_first", TOK_LSB_FIRST},
    {"byte_order", TOK_BYTE_ORDER},
    {"big_endian", TOK_BIG_ENDIAN},
    {"little_endian", TOK_LITTLE_ENDIAN},
};

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_ident_start(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_ident_continue(char c) { return is_ident_start(c) || is_digit(c); }

void lexer_init(Lexer *lexer, const char *source, DiagList *diags) {
  lexer->source = source;
  lexer->position = 0;
  lexer->line = 1;
  lexer->column = 1;
  lexer->diags = diags;
}

static char peek(const Lexer *lexer, size_t ahead) {
  return lexer->source[lexer->position + ahead];
}

static char advance(Lexer *lexer) {
  char c = lexer->source[lexer->position++];
  if (c == '\n') {
    lexer->line += 1;
    lexer->column = 1;
  } else {
    lexer->column += 1;
  }
  return c;
}

static Token make(TokenKind kind, int line, int column) {
  return (Token){.kind = kind, .line = line, .column = column};
}

/* Whitespace and comments separate tokens and are otherwise discarded. An unterminated block comment
 * is reported against the line it opened on: that is the line someone has to go and fix, and by the
 * time the scan reaches end of file the opening is long out of sight. */
static void skip_trivia(Lexer *lexer) {
  for (;;) {
    char c = peek(lexer, 0);

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(lexer);
      continue;
    }

    if (c == '/' && peek(lexer, 1) == '/') {
      while (peek(lexer, 0) != '\0' && peek(lexer, 0) != '\n') {
        advance(lexer);
      }
      continue;
    }

    if (c == '/' && peek(lexer, 1) == '*') {
      int open_line = lexer->line;
      int open_column = lexer->column;
      advance(lexer);
      advance(lexer);

      for (;;) {
        if (peek(lexer, 0) == '\0') {
          diag_add(lexer->diags, open_line, open_column, 0,
                   "unterminated block comment, opened here and never closed");
          return;
        }
        if (peek(lexer, 0) == '*' && peek(lexer, 1) == '/') {
          advance(lexer);
          advance(lexer);
          break;
        }
        advance(lexer);
      }
      continue;
    }

    return;
  }
}

static Token lex_number(Lexer *lexer, int line, int column) {
  Token token = make(TOK_INT, line, column);
  size_t start = lexer->position;
  int base = 10;

  if (peek(lexer, 0) == '0' && (peek(lexer, 1) == 'x' || peek(lexer, 1) == 'X')) {
    base = 16;
    advance(lexer);
    advance(lexer);
    start = lexer->position;
    if (!is_hex_digit(peek(lexer, 0))) {
      diag_add(lexer->diags, line, column, 0, "0x must be followed by at least one hex digit");
      token.kind = TOK_INVALID;
      return token;
    }
    while (is_hex_digit(peek(lexer, 0))) {
      advance(lexer);
    }
  } else if (peek(lexer, 0) == '0' && (peek(lexer, 1) == 'b' || peek(lexer, 1) == 'B')) {
    base = 2;
    advance(lexer);
    advance(lexer);
    start = lexer->position;
    if (peek(lexer, 0) != '0' && peek(lexer, 0) != '1') {
      diag_add(lexer->diags, line, column, 0, "0b must be followed by at least one binary digit");
      token.kind = TOK_INVALID;
      return token;
    }
    while (peek(lexer, 0) == '0' || peek(lexer, 0) == '1') {
      advance(lexer);
    }
  } else {
    while (is_digit(peek(lexer, 0))) {
      advance(lexer);
    }
  }

  /* A number running straight into an identifier character is a typo rather than two tokens:
   * `8bits` should say so, not lex as 8 followed by `bits`. */
  if (is_ident_start(peek(lexer, 0))) {
    diag_add(lexer->diags, lexer->line, lexer->column, 0,
             "unexpected character '%c' immediately after a number", peek(lexer, 0));
    token.kind = TOK_INVALID;
    return token;
  }

  uint64_t value = 0;
  for (size_t i = start; i < lexer->position; ++i) {
    char c = lexer->source[i];
    unsigned digit;
    if (is_digit(c)) {
      digit = (unsigned)(c - '0');
    } else if (c >= 'a') {
      digit = (unsigned)(c - 'a' + 10);
    } else {
      digit = (unsigned)(c - 'A' + 10);
    }

    if (value > (UINT64_MAX - digit) / (uint64_t)base) {
      diag_add(lexer->diags, line, column, 0, "integer literal does not fit in 64 bits");
      token.kind = TOK_INVALID;
      return token;
    }
    value = value * (uint64_t)base + digit;
  }

  token.value = value;
  token.text = lexer->source + start;
  token.length = lexer->position - start;
  return token;
}

static Token lex_word(Lexer *lexer, int line, int column) {
  size_t start = lexer->position;
  while (is_ident_continue(peek(lexer, 0))) {
    advance(lexer);
  }

  size_t length = lexer->position - start;
  const char *text = lexer->source + start;

  for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(*KEYWORDS); ++i) {
    if (strlen(KEYWORDS[i].spelling) == length && memcmp(KEYWORDS[i].spelling, text, length) == 0) {
      Token token = make(KEYWORDS[i].kind, line, column);
      token.text = text;
      token.length = length;
      return token;
    }
  }

  Token token = make(TOK_IDENT, line, column);
  token.text = text;
  token.length = length;
  return token;
}

Token lexer_next(Lexer *lexer) {
  skip_trivia(lexer);

  int line = lexer->line;
  int column = lexer->column;
  char c = peek(lexer, 0);

  if (c == '\0') {
    return make(TOK_EOF, line, column);
  }
  if (is_digit(c)) {
    return lex_number(lexer, line, column);
  }
  if (is_ident_start(c)) {
    return lex_word(lexer, line, column);
  }

  advance(lexer);
  switch (c) {
  case ':': return make(TOK_COLON, line, column);
  case ';': return make(TOK_SEMICOLON, line, column);
  case '{': return make(TOK_LBRACE, line, column);
  case '}': return make(TOK_RBRACE, line, column);
  case '(': return make(TOK_LPAREN, line, column);
  case ')': return make(TOK_RPAREN, line, column);
  case '=': return make(TOK_EQUALS, line, column);
  case '+': return make(TOK_PLUS, line, column);
  case '-': return make(TOK_MINUS, line, column);
  case '*': return make(TOK_STAR, line, column);
  case '/': return make(TOK_SLASH, line, column);
  default: break;
  }

  diag_add(lexer->diags, line, column, 0, "unexpected character '%c'", c);
  return make(TOK_INVALID, line, column);
}

const char *token_kind_name(TokenKind kind) {
  switch (kind) {
  case TOK_EOF: return "end of file";
  case TOK_IDENT: return "a name";
  case TOK_INT: return "a number";
  case TOK_PKT: return "'pkt'";
  case TOK_VALOF: return "'valof'";
  case TOK_MAX: return "'max'";
  case TOK_BIT_ORDER: return "'bit_order'";
  case TOK_MSB_FIRST: return "'msb_first'";
  case TOK_LSB_FIRST: return "'lsb_first'";
  case TOK_BYTE_ORDER: return "'byte_order'";
  case TOK_BIG_ENDIAN: return "'big_endian'";
  case TOK_LITTLE_ENDIAN: return "'little_endian'";
  case TOK_COLON: return "':'";
  case TOK_SEMICOLON: return "';'";
  case TOK_LBRACE: return "'{'";
  case TOK_RBRACE: return "'}'";
  case TOK_LPAREN: return "'('";
  case TOK_RPAREN: return "')'";
  case TOK_EQUALS: return "'='";
  case TOK_PLUS: return "'+'";
  case TOK_MINUS: return "'-'";
  case TOK_STAR: return "'*'";
  case TOK_SLASH: return "'/'";
  case TOK_INVALID: return "invalid input";
  }
  return "something";
}

const char *token_describe(const Token *token) {
  static char buffer[64];

  if (token->kind == TOK_IDENT) {
    int length = (int)(token->length < 40 ? token->length : 40);
    snprintf(buffer, sizeof(buffer), "the name '%.*s'", length, token->text);
    return buffer;
  }
  if (token->kind == TOK_INT) {
    snprintf(buffer, sizeof(buffer), "the number %llu", (unsigned long long)token->value);
    return buffer;
  }
  return token_kind_name(token->kind);
}
