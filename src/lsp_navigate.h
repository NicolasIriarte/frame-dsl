#pragma once

/**
 * @file
 * @brief Hover, go to definition and completion: what the language server says about a name.
 *
 * Kept out of lsp.c, which speaks the protocol, so that what a name *means* can be tested without
 * one. Positions here are the compiler's -- lines and byte columns from 1 -- and lsp.c converts.
 *
 * Every answer comes from three views of the same text:
 *
 *   - the tokens, which say what kind of place the cursor is in and survive any amount of breakage;
 *   - the tree the parser recovers, with imports resolved, which says what each name refers to and
 *     where it was declared -- still there, in part, for a file with errors in it;
 *   - the full load, when the file passes every check, which says where a field actually lands.
 *
 * A file that is half typed gets what can be said about it, and nothing is an error: an empty answer
 * is always a legal one.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct Navigator Navigator;

/** @brief A place in a file: the document itself when `file` is NULL, otherwise that path. */
typedef struct {
  char *file;
  int line;
  int column;
  int end_line;
  int end_column;
} NavLocation;

typedef enum {
  NAV_KEYWORD,
  NAV_BLOCK,
  NAV_CAL,
  NAV_FIELD,
  NAV_ATTRIBUTE,
  NAV_VALUE,
  NAV_NAMESPACE,
  /** @brief An enum, as a field's type (#11). Its names are offered as NAV_VALUE. */
  NAV_ENUM,
} NavKind;

typedef struct {
  char *label;
  NavKind kind;
  /** @brief One line on what it is -- a block's width, where a name came from -- or NULL. */
  char *detail;
} NavCompletion;

/**
 * @brief Reads `source` every way the answers need.
 * @param path The file it is, for resolving imports beside it, or NULL for an unsaved buffer.
 * @return NULL only when memory runs out.
 */
Navigator *nav_open(const char *source, const char *path);
void nav_close(Navigator *navigator);

/**
 * @brief Markdown describing the name at (line, column), or NULL when there is nothing to say.
 * @param range Set to the name's own extent when something is returned. Its `file` is always NULL.
 */
char *nav_hover(const Navigator *navigator, int line, int column, NavLocation *range);

/**
 * @brief Where the name at (line, column) is declared. The caller frees `out->file`.
 * @return false when the cursor is not on a name that refers to something.
 */
bool nav_definition(const Navigator *navigator, int line, int column, NavLocation *out);

/**
 * @brief What could be written at (line, column), given what is before it on the line.
 * @return The candidates, which may be none; NULL only when memory runs out. Free with
 * nav_completions_free().
 */
NavCompletion *nav_complete(const Navigator *navigator, int line, int column, size_t *count);
void nav_completions_free(NavCompletion *items, size_t count);

/** @brief The attribute names the parser knows, NULL-terminated, in the order it lists them. */
extern const char *const NAV_ATTRIBUTE_NAMES[];
