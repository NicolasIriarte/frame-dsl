#pragma once

/**
 * @file
 * @brief What each token of a document is, for semantic highlighting.
 *
 * Classified from the tokens and where they stand, not from a successful parse: an editor asks while
 * the file is half typed, and a keyword three lines above a missing `;` is still a keyword. The words
 * come from the lexer, so a new keyword is highlighted the day the lexer learns it.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lexer.h"

/** @brief In the order of the legend the server announces; the protocol refers to them by index. */
typedef enum {
  SEM_KEYWORD,
  SEM_TYPE,
  SEM_STRUCT,
  SEM_PROPERTY,
  SEM_FUNCTION,
  SEM_PARAMETER,
  SEM_NAMESPACE,
  SEM_DECORATOR,
  SEM_ENUM_MEMBER,
  SEM_NUMBER,
  SEM_STRING,
  SEM_COMMENT,
  SEM_OPERATOR,
  /** @brief An enum's name, where it is declared and where a field is typed by it (#11). */
  SEM_ENUM,
  SEM_TYPE_COUNT,
} SemanticType;

extern const char *const SEMANTIC_TYPE_NAMES[SEM_TYPE_COUNT];

typedef struct {
  /** @brief 1-based, and the column in bytes, as the compiler counts. */
  int line;
  int column;
  /** @brief Where the token is in the source, and how many bytes it is. */
  const char *text;
  size_t length;
  SemanticType type;
  /** @brief The place a name is declared, rather than used. */
  bool declaration;
} SemanticToken;

/**
 * @brief Every token of `source` worth colouring, in order.
 * @param source The document's text, terminated.
 * @param count How many were found.
 * @return Allocated; NULL with `count` 0 if there were none or an allocation failed.
 */
SemanticToken *semantic_tokens(const char *source, size_t *count);

/**
 * @brief Every token of `source`, comments included, each with its exact spelling; what cannot be a
 * token is stepped over. The same scan semantic_tokens() classifies, for the navigation to read.
 * @return NULL when memory runs out, or when there are no tokens at all. Free with free().
 */
Token *lsp_lex_all(const char *source, size_t *count);
