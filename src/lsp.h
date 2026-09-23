#pragma once

/**
 * @file
 * @brief The language server (#30): `frame-dsl --lsp`.
 *
 * JSON-RPC over a pair of streams, speaking the Language Server Protocol to an editor. It holds the
 * documents the editor has open and answers from the same loader, checker and formatter the command
 * line uses -- so an editor cannot be told something about a file that `frame-dsl --check` would not
 * say, and there is no second description of the language to keep in step.
 *
 * Unlike the rest of the command-line tool, this reaches inside the library: semantic tokens need
 * the lexer, and nothing a program linking the library wants is served by making the lexer public.
 */

#include <stdio.h>

/**
 * @brief Serves one editor until it says `exit` or closes the input.
 * @param in Where messages arrive: the editor's end of the conversation.
 * @param out Where replies go. Nothing but protocol is ever written here.
 * @return The process exit status the protocol asks for: 0 after `shutdown` then `exit`, 1 otherwise.
 */
int lsp_serve(FILE *in, FILE *out);
