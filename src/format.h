#pragma once

/**
 * @file
 * @brief The formatter: one layout for every definition file, with no options.
 *
 * It works on the token stream, comments included, rather than on the tree: the tree has no comments
 * and no spelling (`0xCAFE` is a number to it), and a formatter that lost either would be one nobody
 * could run. The parser is only asked whether the file is well formed; a file that is not is refused,
 * not guessed at. The layout itself is described in manual/language.md.
 */

#include <stdbool.h>

#include "diag.h"

/**
 * @brief Format `source`.
 * @param source The text of a definition file.
 * @param diags Syntax errors go here when the source does not parse; nothing else is reported.
 * @return The formatted text, allocated, to be freed; NULL if the source does not parse (the errors
 * say why) or an allocation failed (they do not).
 */
char *format_source(const char *source, DiagList *diags);
