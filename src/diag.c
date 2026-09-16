#include "diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void diag_init(DiagList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
  list->overflowed = false;
}

void diag_free(DiagList *list) {
  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i].message);
  }
  free(list->items);
  diag_init(list);
}

void diag_add(DiagList *list, int line, int column, int check, const char *fmt, ...) {
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

  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  if (len < 0) {
    return;
  }

  char *message = malloc((size_t)len + 1);
  if (message == NULL) {
    return;
  }
  va_start(args, fmt);
  vsnprintf(message, (size_t)len + 1, fmt, args);
  va_end(args);

  list->items[list->count++] = (Diag){ .line = line, .column = column, .check = check, .message = message };
}

static int by_position(const void *lhs, const void *rhs) {
  const Diag *a = lhs;
  const Diag *b = rhs;
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
