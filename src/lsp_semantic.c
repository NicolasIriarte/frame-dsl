#include "lsp_semantic.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "lexer.h"

const char *const SEMANTIC_TYPE_NAMES[SEM_TYPE_COUNT] = {
  "keyword",   "type",       "struct", "property", "function", "parameter", "namespace",
  "decorator", "enumMember", "number", "string",   "comment",  "operator",  "enum",
};

/* Every token, comments included, gathered first so a name can be classified by what follows it. */
Token *lsp_lex_all(const char *source, size_t *count) {
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

/* Whether an identifier is written as a type is: `i16`, `f32`, or a shape check 44 will refuse. */
static bool names_a_type(const Token *token) {
  char name[8];
  if (token->length >= sizeof(name)) {
    return false;
  }
  memcpy(name, token->text, token->length);
  name[token->length] = '\0';
  FieldKind kind = FIELD_KIND_UNSIGNED;
  uint64_t width = 0;
  return ast_type_name(name, &kind, &width);
}

static bool is(const Token *token, TokenKind kind) {
  return token != NULL && token->kind == kind;
}

typedef enum {
  BRACE_BODY,
  BRACE_OVERRIDES,
  /** @brief An enum's names (#11). */
  BRACE_ENUM,
} BraceKind;

/* Whether `name` is an enum this file declares, `enum NAME:` -- which is what tells `level: TxLevel;`
 * from `hdr: Header;`, the same shape. One imported under a namespace is not known here, and reads as
 * a block would. */
static bool declares_enum(const Token *tokens, size_t count, const Token *name) {
  for (size_t i = 0; i + 2 < count; ++i) {
    if (is_word(&tokens[i], "enum") && tokens[i + 1].kind == TOK_IDENT && tokens[i + 2].kind == TOK_COLON
        && tokens[i + 1].length == name->length && memcmp(tokens[i + 1].text, name->text, name->length) == 0) {
      return true;
    }
  }
  return false;
}

typedef enum {
  ATTRIBUTE_NONE,
  ATTRIBUTE_AT,
  ATTRIBUTE_NAME,
  ATTRIBUTE_VALUE,
} AttributeState;

SemanticToken *semantic_tokens(const char *source, size_t *count) {
  *count = 0;
  size_t token_count = 0;
  Token *tokens = lsp_lex_all(source, &token_count);
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
  /* Between `enum` and the `{` of its names. */
  bool in_enum_header = false;
  AttributeState attribute = ATTRIBUTE_NONE;
  bool attribute_is_cal = false;
  /* Inside `valof( ... )`: every part of the name there is a field's, `valof(hdr.len)` (#12), in a width
   * and in an `@if` alike. */
  bool in_valof = false;

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
    case TOK_VALOF:
      in_valof = true;
      type = SEM_KEYWORD;
      break;
    case TOK_PKT:
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
    case TOK_COLON_EQUALS:
    case TOK_QUESTION_EQUALS:
    case TOK_EQUAL_EQUAL:
    case TOK_NOT_EQUAL:
    case TOK_LESS:
    case TOK_LESS_EQUAL:
    case TOK_GREATER:
    case TOK_GREATER_EQUAL:
    case TOK_AND_AND:
    case TOK_OR_OR:
    case TOK_AMPERSAND:
      type = SEM_OPERATOR;
      break;
    case TOK_LBRACE:
      if (depth < (int)(sizeof(braces) / sizeof(*braces))) {
        braces[depth] = in_enum_header ? BRACE_ENUM : in_width ? BRACE_OVERRIDES : BRACE_BODY;
      }
      in_enum_header = false;
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
      in_valof = false;
      break;
    case TOK_IDENT: {
      const BraceKind inside = depth > 0 ? braces[depth - 1 < 64 ? depth - 1 : 63] : BRACE_BODY;
      if (in_valof) {
        type = SEM_PROPERTY;
      } else if (attribute == ATTRIBUTE_AT) {
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
        } else if (statement_start && is_word(token, "enum") && is(next, TOK_IDENT)) {
          type = SEM_KEYWORD;
          in_enum_header = true;
        } else if (in_enum_header && is_word(previous, "enum")) {
          type = SEM_ENUM;
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
      } else if (inside == BRACE_ENUM) {
        type = SEM_ENUM_MEMBER;
        declaration = true;
      } else if (inside == BRACE_OVERRIDES) {
        /* `{ type = HK; }`: the field fixed, and the name of its enum it is fixed to. */
        type = token_is_value_operator(previous) ? SEM_ENUM_MEMBER : SEM_PROPERTY;
      } else if (in_width) {
        if (is(previous, TOK_LPAREN) && is(before_previous, TOK_VALOF)) {
          type = SEM_PROPERTY;
        } else if (token_is_value_operator(previous)) {
          type = SEM_ENUM_MEMBER; /* `level: TxLevel := HIGH;` */
        } else if (names_a_type(token)) {
          type = SEM_TYPE; /* `temp: i16;` */
        } else if (is(next, TOK_DOT)) {
          type = SEM_NAMESPACE;
        } else {
          type = declares_enum(tokens, token_count, token) ? SEM_ENUM : SEM_STRUCT;
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
