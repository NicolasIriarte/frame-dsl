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
