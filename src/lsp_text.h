#pragma once

/**
 * @file
 * @brief Positions in a document, both ways between the compiler's and the protocol's.
 *
 * The compiler counts lines and columns from 1, and a column in bytes. The protocol counts from 0, and
 * a "character" in whichever encoding was agreed at initialisation: UTF-8 code units (bytes) when the
 * editor offers them, UTF-16 code units otherwise -- which is the protocol's default, and how an
 * accented letter in a comment ends up one column wide to the editor and two to the compiler.
 */

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  const char *text;
  size_t length;
  /** @brief Where each line starts, as a byte offset; `line_count` entries, the first 0. */
  size_t *starts;
  size_t line_count;
  bool utf16;
} LspText;

/** @return false if the line table could not be allocated. */
bool lsp_text_init(LspText *text, const char *source, size_t length, bool utf16);
void lsp_text_free(LspText *text);

/** @brief The protocol's character for a byte offset within `line` (0-based). */
size_t lsp_text_character(const LspText *text, size_t line, size_t byte_offset_in_line);

/** @brief The protocol's position for the compiler's 1-based line and byte column. Clamped to the text. */
void lsp_text_position(const LspText *text, int line, int column, size_t *out_line, size_t *out_character);

/** @brief The byte offset in the whole text of a protocol position. Clamped to the text. */
size_t lsp_text_offset(const LspText *text, size_t line, size_t character);

/** @brief How many units `length` bytes starting at `from` are in the agreed encoding. */
size_t lsp_text_units(const LspText *text, const char *from, size_t length);
