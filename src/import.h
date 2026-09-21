#pragma once

/**
 * @file
 * @brief Imports: blocks and calibrations brought in from other definition files.
 *
 * Runs between the parser and composition. Each imported file is read, parsed, has its own imports
 * resolved, and is composed and checked on its own -- so a mistake in it is reported once, in its own
 * file, however many files import it. Then its blocks and calibrations are merged into the importing
 * file, ahead of the file's own declarations, renamed under `alias.` when the import has one. Its
 * packets are not: an import never changes which packets a definition decodes.
 *
 * What is merged is merged by pointer. The imported file's tree is kept alive by the importing one
 * (AstFile.imported) and freed with it.
 */

#include <stdbool.h>

#include "ast.h"
#include "diag.h"

/**
 * @brief Resolves every import of `file`, recursively, and merges what they bring.
 * @param file A file that parsed without errors.
 * @param base_dir The directory relative paths are resolved against: the importing file's own, or
 * "." for a definition loaded from a string.
 * @param self The real path of `file` itself, or NULL when it was not read from a file. Used to
 * recognise an import cycle that comes back to the file being loaded.
 * @param diags Reported into: checks 26 to 28, and every problem of an imported file, attributed to
 * that file.
 * @return false if anything was reported.
 */
bool imports_resolve(AstFile *file, const char *base_dir, const char *self, DiagList *diags);
