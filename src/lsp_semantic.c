#include "lsp_semantic.h"

#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "lexer.h"

const char *const SEMANTIC_TYPE_NAMES[SEM_TYPE_COUNT] = {
  "keyword",   "type",       "struct", "property", "function", "parameter", "namespace",
  "decorator", "enumMember", "number", "string",   "comment",  "operator",
};

/* Every token, comments included, gathered first so a name can be classified by what follows it. */
static Token *lex_all(const char *source, size_t *count) {
  size_t capacity = 256;
  Token *tokens = malloc(capacity * sizeof(*tokens));
  if (tokens == NULL) {
    return NULL;
  }
  DiagList scratch;
  diag_init(&scratch);
  Lexer lexer;
  lexer_init(&lexer, source, &scratch);

  *count = 0;
  for (;;) {
    size_t before = lexer.position;
    Token token = lexer_next_spelled(&lexer);
    if (token.kind == TOK_EOF) {
      break;
    }
    /* A half-typed file is the usual case here, so what cannot be a token is stepped over rather than
     * ending the scan. The lexer has already moved past it; the guard is only against an input that
     * would leave it standing still, which would be a loop that never ends. */
    if (token.kind == TOK_INVALID) {
      if (lexer.position == before) {
        lexer.position += 1;
        lexer.column += 1;
      }
      continue;
    }
    if (*count == capacity) {
      capacity *= 2;
      Token *grown = realloc(tokens, capacity * sizeof(*grown));
      if (grown == NULL) {
        free(tokens);
        diag_free(&scratch);
        return NULL;
      }
      tokens = grown;
    }
    tokens[(*count)++] = token;
  }
  diag_free(&scratch);
  return tokens;
}

static bool is_word(const Token *token, const char *word) {
  size_t length = strlen(word);
  return token != NULL && token->kind == TOK_IDENT && token->length == length && memcmp(token->text, word, length) == 0;
}

static bool is(const Token *token, TokenKind kind) {
  return token != NULL && token->kind == kind;
}

typedef enum {
  BRACE_BODY,
  BRACE_OVERRIDES,
} BraceKind;

typedef enum {
  ATTRIBUTE_NONE,
  ATTRIBUTE_AT,
  ATTRIBUTE_NAME,
  ATTRIBUTE_VALUE,
} AttributeState;

SemanticToken *semantic_tokens(const char *source, size_t *count) {
  *count = 0;
  size_t token_count = 0;
  Token *tokens = lex_all(source, &token_count);
  if (tokens == NULL || token_count == 0) {
    free(tokens);
    return NULL;
  }
  SemanticToken *out = malloc(token_count * sizeof(*out));
  if (out == NULL) {
    free(tokens);
    return NULL;
  }

  BraceKind braces[64];
  int depth = 0;
  int parens = 0;
  bool in_width = false;
  bool in_cal = false;
  AttributeState attribute = ATTRIBUTE_NONE;
  bool attribute_is_cal = false;

  /* The previous two significant tokens, and the next one: all a name's meaning depends on. */
  const Token *previous = NULL;
  const Token *before_previous = NULL;
  const Token *third_previous = NULL;

  for (size_t i = 0; i < token_count; ++i) {
    const Token *token = &tokens[i];
    if (token->kind == TOK_COMMENT) {
      out[(*count)++] = (SemanticToken){ token->line, token->column, token->text, token->length, SEM_COMMENT, false };
      continue;
    }
    const Token *next = NULL;
    for (size_t j = i + 1; j < token_count; ++j) {
      if (tokens[j].kind != TOK_COMMENT) {
        next = &tokens[j];
        break;
      }
    }

    SemanticType type = SEM_TYPE_COUNT;
    bool declaration = false;
    const bool statement_start = previous == NULL || is(previous, TOK_SEMICOLON);

    switch (token->kind) {
    case TOK_PKT:
    case TOK_VALOF:
    case TOK_BLOCK:
      type = SEM_KEYWORD;
      break;
    case TOK_INT:
    case TOK_REAL:
      type = SEM_NUMBER;
      break;
    case TOK_STRING:
      type = SEM_STRING;
      break;
    case TOK_AT:
      type = SEM_DECORATOR;
      attribute = ATTRIBUTE_AT;
      break;
    case TOK_PLUS:
    case TOK_MINUS:
    case TOK_STAR:
    case TOK_SLASH:
    case TOK_EQUALS:
      type = SEM_OPERATOR;
      break;
    case TOK_LBRACE:
      if (depth < (int)(sizeof(braces) / sizeof(*braces))) {
        braces[depth] = in_width ? BRACE_OVERRIDES : BRACE_BODY;
      }
      depth += 1;
      break;
    case TOK_RBRACE:
      depth = depth > 0 ? depth - 1 : 0;
      break;
    case TOK_SEMICOLON:
      if (parens == 0) {
        in_width = false;
        in_cal = false;
        attribute = ATTRIBUTE_NONE;
      }
      break;
    case TOK_COLON:
      if (attribute == ATTRIBUTE_NAME) {
        attribute = ATTRIBUTE_VALUE;
      } else if (depth > 0 && parens == 0 && braces[depth - 1 < 64 ? depth - 1 : 63] == BRACE_BODY) {
        in_width = true;
      }
      break;
    case TOK_LPAREN:
      parens += 1;
      break;
    case TOK_RPAREN:
      parens = parens > 0 ? parens - 1 : 0;
      break;
    case TOK_IDENT: {
      const BraceKind inside = depth > 0 ? braces[depth - 1 < 64 ? depth - 1 : 63] : BRACE_BODY;
      if (attribute == ATTRIBUTE_AT) {
        type = SEM_DECORATOR;
        attribute = ATTRIBUTE_NAME;
        attribute_is_cal = is_word(token, "cal");
      } else if (attribute == ATTRIBUTE_VALUE) {
        type = is(next, TOK_DOT) ? SEM_NAMESPACE : attribute_is_cal ? SEM_FUNCTION : SEM_ENUM_MEMBER;
      } else if (depth == 0) {
        if (statement_start && is_word(token, "import")) {
          type = SEM_KEYWORD;
        } else if (is_word(token, "as") && is(previous, TOK_STRING)) {
          type = SEM_KEYWORD;
        } else if (is_word(previous, "as") && is(before_previous, TOK_STRING)) {
          type = SEM_NAMESPACE;
          declaration = true;
        } else if (statement_start && is_word(token, "cal")) {
          type = SEM_KEYWORD;
          in_cal = true;
        } else if (in_cal) {
          if (is_word(previous, "cal") && (before_previous == NULL || is(before_previous, TOK_SEMICOLON))) {
            type = SEM_FUNCTION;
            declaration = true;
          } else if (parens == 0 && is_word(token, "inverse")) {
            type = SEM_KEYWORD;
          } else if (is(next, TOK_LPAREN)) {
            type = SEM_FUNCTION;
          } else if (is(next, TOK_DOT)) {
            type = SEM_NAMESPACE;
          } else {
            /* Everything else a calibration names is its parameter -- check 19 says so. `cal f(x)` and
             * `inverse (y)` are where the two are declared. */
            type = SEM_PARAMETER;
            declaration =
              is(previous, TOK_LPAREN)
              && (is_word(before_previous, "inverse") || (is(before_previous, TOK_IDENT) && is_word(third_previous, "cal")));
          }
        } else if (is(previous, TOK_PKT)) {
          type = SEM_TYPE;
          declaration = true;
        } else if (is(previous, TOK_BLOCK)) {
          type = SEM_STRUCT;
          declaration = true;
        }
      } else if (inside == BRACE_OVERRIDES) {
        type = SEM_PROPERTY;
      } else if (in_width) {
        if (is(previous, TOK_LPAREN) && is(before_previous, TOK_VALOF)) {
          type = SEM_PROPERTY;
        } else {
          type = is(next, TOK_DOT) ? SEM_NAMESPACE : SEM_STRUCT;
        }
      } else if (is(next, TOK_COLON) || is(next, TOK_LBRACKET)) {
        type = SEM_PROPERTY;
        declaration = true;
      } else if ((previous == NULL || is(previous, TOK_SEMICOLON) || is(previous, TOK_LBRACE)
                  || is(previous, TOK_RBRACKET))
                 && (is(next, TOK_LBRACE) || is(next, TOK_SEMICOLON) || is(next, TOK_DOT))) {
        /* A block pasted in without an instance name, `Header { ... };`: read from here on as the
         * width part of a named use would be, so its overrides and a `ns.` prefix classify the same. */
        in_width = true;
        type = is(next, TOK_DOT) ? SEM_NAMESPACE : SEM_STRUCT;
      }
      break;
    }
    default:
      break;
    }

    if (type != SEM_TYPE_COUNT) {
      out[(*count)++] = (SemanticToken){ token->line, token->column, token->text, token->length, type, declaration };
    }
    third_previous = before_previous;
    before_previous = previous;
    previous = token;
  }

  free(tokens);
  return out;
}
