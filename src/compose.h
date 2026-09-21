#pragma once

/**
 * @file
 * @brief Composition: named blocks copied into the packets that use them.
 *
 * Runs between the parser and the checks. Every field whose width is a block's name is replaced by the
 * block's fields, renamed under the instance (`hdr.id`), with the `valof()` references inside renamed
 * to match and the use's overrides applied as fixed values. What comes out is packets of ordinary
 * fields, so nothing after this -- the checks, the layout, the bytecode, the encoder, the decoder --
 * knows composition exists, and an instance costs nothing at run time that writing its fields out
 * would not.
 */

#include <stdbool.h>

#include "ast.h"
#include "diag.h"

/**
 * @brief Expands every block use in `file`, in place.
 * @param file A file that parsed without errors.
 * @param diags Reported into: checks 23 to 25, and an allocation failure.
 * @return false if anything was reported, in which case the file must not be checked.
 */
bool compose_file(AstFile *file, DiagList *diags);
