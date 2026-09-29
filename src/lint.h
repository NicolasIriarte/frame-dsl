#pragma once

/**
 * @file
 * @brief Warnings: what loads fine and is almost certainly not what was meant.
 *
 * Nothing here fails a load. The attribute warnings -- redundant (check 31) and written twice (check 34)
 * -- are the parser's, since that is where attribute scopes are flattened and the only place both
 * values are still in sight; this is the rest, which needs the whole file and what its imports brought.
 */

#include "ast.h"
#include "diag.h"

/**
 * @brief Check 30: a block, calibration or import declared in `file` and used by nothing.
 *
 * Only in a file with at least one packet. A file of blocks and calibrations and no packets is a
 * library, and its declarations -- what it imports included, since that is merged into what it offers
 * -- are its exports: nothing in the file itself is meant to use them.
 *
 * Something counts as used when anything declared in this file names it, used or not. So of a block
 * that only an unused block uses, only the outer one is reported: removing it is what makes the inner
 * one unused, and saying so now would be two warnings for one mistake.
 *
 * @param file A file whose imports have been resolved and whose blocks are not yet expanded: after
 * compose_file() nothing names a block any more.
 * @param diags Warnings are added here.
 */
void lint_unused(const AstFile *file, DiagList *diags);
