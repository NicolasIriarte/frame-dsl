#include "lexer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *spelling;
  TokenKind kind;
} Keyword;

static const Keyword KEYWORDS[] = {
  { "pkt",   TOK_PKT   },
  { "valof", TOK_VALOF },
  { "block", TOK_BLOCK },
};

static bool is_digit(char c) {
  return c >= '0' && c <= '9';
}
static bool is_hex_digit(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static bool is_ident_start(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool is_ident_continue(char c) {
  return is_ident_start(c) || is_digit(c);
}

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
  return (Token){ .kind = kind, .line = line, .column = column };
}

/* Whitespace and comments separate tokens and are otherwise discarded.
 *
 * A comment runs from '#' to the end of the line, and that is the only form there is. There is no
 * block comment: a definition file is short, every editor comments a region by prefixing its lines,
 * and one syntax is easier to state than two. It also leaves '/' meaning division and nothing else,
 * so the lexer no longer has to look ahead to find out which of two things a slash is. */
static void skip_trivia(Lexer *lexer) {
  for (;;) {
    char c = peek(lexer, 0);

    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      advance(lexer);
      continue;
    }

    if (c == '#') {
      while (peek(lexer, 0) != '\0' && peek(lexer, 0) != '\n') {
        advance(lexer);
      }
      continue;
    }

    return;
  }
}

/* The rest of a decimal real, from `start`, which is where its integer digits began. strtod() does the
 * conversion: correctly rounding a decimal to a double is not something to write by hand. */
static Token lex_real(Lexer *lexer, int line, int column, size_t start) {
  Token token = make(TOK_REAL, line, column);

  if (peek(lexer, 0) == '.') {
    advance(lexer);
    while (is_digit(peek(lexer, 0))) {
      advance(lexer);
    }
  }
  if (peek(lexer, 0) == 'e' || peek(lexer, 0) == 'E') {
    advance(lexer);
    if (peek(lexer, 0) == '+' || peek(lexer, 0) == '-') {
      advance(lexer);
    }
    while (is_digit(peek(lexer, 0))) {
      advance(lexer);
    }
  }

  if (is_ident_start(peek(lexer, 0)) || peek(lexer, 0) == '.') {
    diag_add(
      lexer->diags,
      lexer->line,
      lexer->column,
      0,
      "unexpected character '%c' immediately after a number",
      peek(lexer, 0)
    );
    token.kind = TOK_INVALID;
    return token;
  }

  char digits[64];
  size_t length = lexer->position - start;
  if (length >= sizeof(digits)) {
    diag_add(lexer->diags, line, column, 0, "a real literal of more than %zu characters", sizeof(digits) - 1);
    token.kind = TOK_INVALID;
    return token;
  }
  memcpy(digits, lexer->source + start, length);
  digits[length] = '\0';

  token.real = strtod(digits, NULL);
  if (!isfinite(token.real)) {
    diag_add(lexer->diags, line, column, 0, "real literal is too large for a double");
    token.kind = TOK_INVALID;
    return token;
  }
  token.text = lexer->source + start;
  token.length = length;
  return token;
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

    /* A fraction or an exponent makes it a real, which only a calibration accepts. Both need a digit
     * after them, so `8.` stays an error rather than quietly becoming 8.0. */
    bool fraction = peek(lexer, 0) == '.' && is_digit(peek(lexer, 1));
    bool exponent =
      (peek(lexer, 0) == 'e' || peek(lexer, 0) == 'E')
      && (is_digit(peek(lexer, 1)) || ((peek(lexer, 1) == '+' || peek(lexer, 1) == '-') && is_digit(peek(lexer, 2))));
    if (fraction || exponent) {
      return lex_real(lexer, line, column, start);
    }
    /* `8.` is a typo for 8 or for 8.something, and never the start of an override path -- a path
     * starts with a name. */
    if (peek(lexer, 0) == '.') {
      diag_add(lexer->diags, lexer->line, lexer->column, 0, "a number cannot end in '.'; write the digits after it");
      token.kind = TOK_INVALID;
      return token;
    }
  }

  /* A number running straight into an identifier character is a typo rather than two tokens:
   * `8bits` should say so, not lex as 8 followed by `bits`. */
  if (is_ident_start(peek(lexer, 0))) {
    diag_add(
      lexer->diags,
      lexer->line,
      lexer->column,
      0,
      "unexpected character '%c' immediately after a number",
      peek(lexer, 0)
    );
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
  token.real = (double)value;
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

  /* A path, and only a path: no escapes, and no line breaks, since nothing else in the language is
   * text and a file name has no need of either. */
  if (c == '"') {
    advance(lexer);
    size_t start = lexer->position;
    while (peek(lexer, 0) != '"' && peek(lexer, 0) != '\n' && peek(lexer, 0) != '\0') {
      advance(lexer);
    }
    if (peek(lexer, 0) != '"') {
      diag_add(lexer->diags, line, column, 0, "a string must close on the line it opens");
      return make(TOK_INVALID, line, column);
    }
    Token token = make(TOK_STRING, line, column);
    token.text = lexer->source + start;
    token.length = lexer->position - start;
    advance(lexer);
    return token;
  }

  advance(lexer);
  switch (c) {
  case '@':
    return make(TOK_AT, line, column);
  case ':':
    return make(TOK_COLON, line, column);
  case ';':
    return make(TOK_SEMICOLON, line, column);
  case '{':
    return make(TOK_LBRACE, line, column);
  case '}':
    return make(TOK_RBRACE, line, column);
  case '[':
    return make(TOK_LBRACKET, line, column);
  case ']':
    return make(TOK_RBRACKET, line, column);
  case '(':
    return make(TOK_LPAREN, line, column);
  case ')':
    return make(TOK_RPAREN, line, column);
  case '=':
    if (peek(lexer, 0) == '=') {
      advance(lexer);
      return make(TOK_EQUAL_EQUAL, line, column);
    }
    return make(TOK_EQUALS, line, column);
  case '!':
    if (peek(lexer, 0) == '=') {
      advance(lexer);
      return make(TOK_NOT_EQUAL, line, column);
    }
    break;
  case '<':
    if (peek(lexer, 0) == '=') {
      advance(lexer);
      return make(TOK_LESS_EQUAL, line, column);
    }
    return make(TOK_LESS, line, column);
  case '>':
    if (peek(lexer, 0) == '=') {
      advance(lexer);
      return make(TOK_GREATER_EQUAL, line, column);
    }
    return make(TOK_GREATER, line, column);
  case '&':
    if (peek(lexer, 0) == '&') {
      advance(lexer);
      return make(TOK_AND_AND, line, column);
    }
    return make(TOK_AMPERSAND, line, column);
  case '|':
    if (peek(lexer, 0) == '|') {
      advance(lexer);
      return make(TOK_OR_OR, line, column);
    }
    /* There is no bitwise OR: a condition masks with '&', and combines with '||'. */
    diag_add(lexer->diags, line, column, 0, "unexpected character '|'; conditions are combined with '||'");
    return make(TOK_INVALID, line, column);
  case '+':
    return make(TOK_PLUS, line, column);
  case '-':
    return make(TOK_MINUS, line, column);
  case '*':
    return make(TOK_STAR, line, column);
  case '/':
    return make(TOK_SLASH, line, column);
  case '.':
    return make(TOK_DOT, line, column);
  default:
    break;
  }

  diag_add(lexer->diags, line, column, 0, "unexpected character '%c'", c);
  return make(TOK_INVALID, line, column);
}

Token lexer_next_spelled(Lexer *lexer) {
  while (peek(lexer, 0) == ' ' || peek(lexer, 0) == '\t' || peek(lexer, 0) == '\r' || peek(lexer, 0) == '\n') {
    advance(lexer);
  }

  size_t start = lexer->position;
  Token token;
  if (peek(lexer, 0) == '#') {
    token = make(TOK_COMMENT, lexer->line, lexer->column);
    while (peek(lexer, 0) != '\0' && peek(lexer, 0) != '\n') {
      advance(lexer);
    }
  } else {
    token = lexer_next(lexer);
  }

  /* A comment's trailing spaces are not part of what it says, and would otherwise survive every
   * reformat as invisible noise at the end of a line. */
  size_t end = lexer->position;
  if (token.kind == TOK_COMMENT) {
    while (end > start
           && (lexer->source[end - 1] == ' ' || lexer->source[end - 1] == '\t' || lexer->source[end - 1] == '\r')) {
      end -= 1;
    }
  }
  token.text = lexer->source + start;
  token.length = end - start;
  return token;
}

const char *token_kind_name(TokenKind kind) {
  switch (kind) {
  case TOK_EOF:
    return "end of file";
  case TOK_IDENT:
    return "a name";
  case TOK_INT:
    return "a number";
  case TOK_REAL:
    return "a real number";
  case TOK_PKT:
    return "'pkt'";
  case TOK_VALOF:
    return "'valof'";
  case TOK_BLOCK:
    return "'block'";
  case TOK_AT:
    return "'@'";
  case TOK_COLON:
    return "':'";
  case TOK_SEMICOLON:
    return "';'";
  case TOK_LBRACE:
    return "'{'";
  case TOK_RBRACE:
    return "'}'";
  case TOK_LBRACKET:
    return "'['";
  case TOK_RBRACKET:
    return "']'";
  case TOK_LPAREN:
    return "'('";
  case TOK_RPAREN:
    return "')'";
  case TOK_EQUALS:
    return "'='";
  case TOK_EQUAL_EQUAL:
    return "'=='";
  case TOK_NOT_EQUAL:
    return "'!='";
  case TOK_LESS:
    return "'<'";
  case TOK_LESS_EQUAL:
    return "'<='";
  case TOK_GREATER:
    return "'>'";
  case TOK_GREATER_EQUAL:
    return "'>='";
  case TOK_AND_AND:
    return "'&&'";
  case TOK_OR_OR:
    return "'||'";
  case TOK_AMPERSAND:
    return "'&'";
  case TOK_PLUS:
    return "'+'";
  case TOK_MINUS:
    return "'-'";
  case TOK_STAR:
    return "'*'";
  case TOK_SLASH:
    return "'/'";
  case TOK_DOT:
    return "'.'";
  case TOK_STRING:
    return "a quoted path";
  case TOK_INVALID:
    return "invalid input";
  case TOK_COMMENT:
    return "a comment";
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
  if (token->kind == TOK_REAL) {
    int length = (int)(token->length < 40 ? token->length : 40);
    snprintf(buffer, sizeof(buffer), "the number %.*s", length, token->text);
    return buffer;
  }
  if (token->kind == TOK_INT) {
    snprintf(buffer, sizeof(buffer), "the number %llu", (unsigned long long)token->value);
    return buffer;
  }
  return token_kind_name(token->kind);
}
