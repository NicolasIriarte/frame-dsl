#pragma once

#include "ast.h"
#include "diag.h"

/* Parses `source` per docs/grammar.ebnf.
 *
 * Returns the file it managed to build, or NULL if it could not allocate. A non-NULL return does not
 * mean success: check `diags`. On a syntax error the parser records it, skips to the next point it
 * can resume from, and carries on, so what comes back may be a partial tree. Nothing downstream
 * should run on it while there are diagnostics.
 *
 * The caller owns the result and frees it with ast_free(). */
AstFile *parse(const char *source, DiagList *diags);
