#include "lsp_text.h"

#include <stdlib.h>

bool lsp_text_init(LspText *text, const char *source, size_t length, bool utf16) {
  text->text = source;
  text->length = length;
  text->utf16 = utf16;
  text->line_count = 1;
  for (size_t i = 0; i < length; ++i) {
    text->line_count += source[i] == '\n';
  }
  text->starts = malloc(text->line_count * sizeof(*text->starts));
  if (text->starts == NULL) {
    text->line_count = 0;
    return false;
  }
  size_t line = 0;
  text->starts[line++] = 0;
  for (size_t i = 0; i < length; ++i) {
    if (source[i] == '\n') {
      text->starts[line++] = i + 1;
    }
  }
  return true;
}

void lsp_text_free(LspText *text) {
  free(text->starts);
  text->starts = NULL;
  text->line_count = 0;
}

/* The length in bytes of the UTF-8 sequence a lead byte starts. A stray continuation byte counts as one,
 * so text that is not UTF-8 still advances. */
static size_t sequence_length(unsigned char lead) {
  if (lead >= 0xF0) {
    return 4;
  }
  if (lead >= 0xE0) {
    return 3;
  }
  if (lead >= 0xC0) {
    return 2;
  }
  return 1;
}

size_t lsp_text_units(const LspText *text, const char *from, size_t length) {
  if (!text->utf16) {
    return length;
  }
  size_t units = 0;
  for (size_t i = 0; i < length;) {
    size_t step = sequence_length((unsigned char)from[i]);
    /* Four bytes of UTF-8 is a code point past the BMP: two UTF-16 units, a surrogate pair. */
    units += step == 4 ? 2 : 1;
    i += step;
  }
  return units;
}

static size_t line_end(const LspText *text, size_t line) {
  size_t end = line + 1 < text->line_count ? text->starts[line + 1] - 1 : text->length;
  return end;
}

size_t lsp_text_character(const LspText *text, size_t line, size_t byte_offset_in_line) {
  if (line >= text->line_count) {
    return 0;
  }
  size_t start = text->starts[line];
  size_t available = line_end(text, line) - start;
  if (byte_offset_in_line > available) {
    byte_offset_in_line = available;
  }
  return lsp_text_units(text, text->text + start, byte_offset_in_line);
}

void lsp_text_position(const LspText *text, int line, int column, size_t *out_line, size_t *out_character) {
  if (line < 1 || text->line_count == 0) {
    *out_line = 0;
    *out_character = 0;
    return;
  }
  size_t index = (size_t)line - 1;
  if (index >= text->line_count) {
    index = text->line_count - 1;
    column = (int)(line_end(text, index) - text->starts[index]) + 1;
  }
  *out_line = index;
  *out_character = lsp_text_character(text, index, column < 1 ? 0 : (size_t)column - 1);
}

size_t lsp_text_offset(const LspText *text, size_t line, size_t character) {
  if (text->line_count == 0) {
    return 0;
  }
  if (line >= text->line_count) {
    return text->length;
  }
  size_t at = text->starts[line];
  size_t end = line_end(text, line);
  size_t units = 0;
  while (at < end && units < character) {
    size_t step = sequence_length((unsigned char)text->text[at]);
    units += text->utf16 && step == 4 ? 2 : (text->utf16 ? 1 : step);
    at += step;
  }
  return at > end ? end : at;
}
