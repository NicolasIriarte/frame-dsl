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
    free(list->items[i].file);
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

  list->items[list->count++] =
    (Diag){ .line = line, .column = column, .check = check, .message = message, .file = NULL };
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
    diag_add(to, item->line, item->column, item->check, "%s", item->message);
    if (to->count == before) {
      continue; /* the list is full, or an allocation failed */
    }

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
