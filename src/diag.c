#include "diag.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"

void diag_init(DiagList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
  list->overflowed = false;
}

void diag_free(DiagList *list) {
  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i].message);
    free(list->items[i].file);
  }
  free(list->items);
  diag_init(list);
}

static void add(DiagList *list, DiagSeverity severity, int line, int column, int check, const char *fmt, va_list args)
  __attribute__((format(printf, 6, 0)));

static void add(DiagList *list, DiagSeverity severity, int line, int column, int check, const char *fmt, va_list args) {
  if (list->count >= DIAG_MAX) {
    list->overflowed = true;
    return;
  }

  if (list->count == list->capacity) {
    size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    Diag *items = realloc(list->items, capacity * sizeof(*items));
    if (items == NULL) {
      return;
    }
    list->items = items;
    list->capacity = capacity;
  }

  va_list measure;
  va_copy(measure, args);
  int len = vsnprintf(NULL, 0, fmt, measure);
  va_end(measure);
  if (len < 0) {
    return;
  }

  char *message = malloc((size_t)len + 1);
  if (message == NULL) {
    return;
  }
  vsnprintf(message, (size_t)len + 1, fmt, args);

  list->items[list->count++] =
    (Diag){ .line = line, .column = column, .severity = severity, .check = check, .message = message, .file = NULL };
}

void diag_add(DiagList *list, int line, int column, int check, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  add(list, DIAG_ERROR, line, column, check, fmt, args);
  va_end(args);
}

void diag_warn(DiagList *list, int line, int column, int check, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  add(list, DIAG_WARNING, line, column, check, fmt, args);
  va_end(args);
}

size_t diag_error_count(const DiagList *list) {
  size_t errors = 0;
  for (size_t i = 0; i < list->count; ++i) {
    errors += list->items[i].severity == DIAG_ERROR;
  }
  return errors;
}

void diag_drop_warnings(DiagList *list) {
  size_t kept = 0;
  for (size_t i = 0; i < list->count; ++i) {
    if (list->items[i].severity == DIAG_WARNING) {
      free(list->items[i].message);
      free(list->items[i].file);
    } else {
      list->items[kept++] = list->items[i];
    }
  }
  list->count = kept;
}

/* Where `line`:`column` is in `source`, as an offset; SIZE_MAX if the source ends first. */
static size_t offset_of(const char *source, int line, int column) {
  size_t offset = 0;
  for (int at = 1; at < line; ++at) {
    const char *newline = strchr(source + offset, '\n');
    if (newline == NULL) {
      return SIZE_MAX;
    }
    offset = (size_t)(newline - source) + 1;
  }
  for (int at = 1; at < column; ++at) {
    if (source[offset] == '\0' || source[offset] == '\n') {
      return SIZE_MAX;
    }
    offset += 1;
  }
  return offset;
}

void diag_fill_spans(DiagList *list, const char *source) {
  for (size_t i = 0; i < list->count; ++i) {
    Diag *item = &list->items[i];
    if (item->file != NULL || item->end_line != 0) {
      continue;
    }
    item->end_line = item->line;
    item->end_column = item->column;

    size_t offset = item->line > 0 ? offset_of(source, item->line, item->column) : SIZE_MAX;
    if (offset == SIZE_MAX) {
      continue;
    }

    /* One token, lexed from exactly here. Whatever the lexer would say about it has been said already,
     * by the load; this list only exists to be thrown away. */
    DiagList scratch;
    diag_init(&scratch);
    Lexer lexer;
    lexer_init(&lexer, source, &scratch);
    lexer.position = offset;
    lexer.line = item->line;
    lexer.column = item->column;
    Token token = lexer_next(&lexer);
    diag_free(&scratch);

    /* A token that starts somewhere else means the position was whitespace or a comment: nothing to
     * cover, so the span stays empty rather than claim the next word. */
    if (token.kind != TOK_EOF && token.line == item->line && token.column == item->column) {
      item->end_line = lexer.line;
      item->end_column = lexer.column;
    }
  }
}

/* Imported files first, since their problems are reported at the top of the file that imports them,
 * then the file itself; by position within each. */
static int by_position(const void *lhs, const void *rhs) {
  const Diag *a = lhs;
  const Diag *b = rhs;
  if ((a->file == NULL) != (b->file == NULL)) {
    return a->file == NULL ? 1 : -1;
  }
  if (a->file != NULL) {
    int order = strcmp(a->file, b->file);
    if (order != 0) {
      return order;
    }
  }
  if (a->line != b->line) {
    return a->line < b->line ? -1 : 1;
  }
  if (a->column != b->column) {
    return a->column < b->column ? -1 : 1;
  }
  return 0;
}

void diag_sort(DiagList *list) {
  if (list->count > 1) {
    qsort(list->items, list->count, sizeof(*list->items), by_position);
  }
}

void diag_append(DiagList *to, const DiagList *from, const char *file) {
  for (size_t i = 0; i < from->count; ++i) {
    const Diag *item = &from->items[i];
    size_t before = to->count;
    if (item->severity == DIAG_ERROR) {
      diag_add(to, item->line, item->column, item->check, "%s", item->message);
    } else {
      diag_warn(to, item->line, item->column, item->check, "%s", item->message);
    }
    if (to->count == before) {
      continue; /* the list is full, or an allocation failed */
    }
    to->items[to->count - 1].end_line = item->end_line;
    to->items[to->count - 1].end_column = item->end_column;

    const char *origin = item->file != NULL ? item->file : file;
    if (origin != NULL) {
      to->items[to->count - 1].file = malloc(strlen(origin) + 1);
      if (to->items[to->count - 1].file != NULL) {
        strcpy(to->items[to->count - 1].file, origin);
      }
    }
  }
  if (from->overflowed) {
    to->overflowed = true;
  }
}
