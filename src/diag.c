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

static void fix_free(DiagFix *fix) {
  if (fix != NULL) {
    free(fix->text);
    free(fix->title);
    free(fix);
  }
}

void diag_free(DiagList *list) {
  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i].message);
    free(list->items[i].file);
    fix_free(list->items[i].fix);
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
    (Diag){ .line = line, .column = column, .severity = severity, .check = check, .message = message };
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
      fix_free(list->items[i].fix);
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

static DiagFix *make_fix(
  int line,
  int column,
  int end_line,
  int end_column,
  const char *text,
  const char *title,
  va_list args
) __attribute__((format(printf, 6, 0)));

static DiagFix *make_fix(
  int line,
  int column,
  int end_line,
  int end_column,
  const char *text,
  const char *title,
  va_list args
) {
  DiagFix *fix = calloc(1, sizeof(*fix));
  if (fix == NULL) {
    return NULL;
  }
  va_list measure;
  va_copy(measure, args);
  int len = vsnprintf(NULL, 0, title, measure);
  va_end(measure);
  fix->text = malloc(strlen(text) + 1);
  fix->title = len >= 0 ? malloc((size_t)len + 1) : NULL;
  if (fix->text == NULL || fix->title == NULL) {
    fix_free(fix);
    return NULL;
  }
  strcpy(fix->text, text);
  vsnprintf(fix->title, (size_t)len + 1, title, args);
  *fix = (DiagFix){
    .line = line,
    .column = column,
    .end_line = end_line,
    .end_column = end_column,
    .text = fix->text,
    .title = fix->title,
  };
  return fix;
}

void diag_set_fix(
  DiagList *list,
  size_t index,
  int line,
  int column,
  int end_line,
  int end_column,
  const char *text,
  const char *title,
  ...
) {
  if (index >= list->count) {
    return;
  }
  va_list args;
  va_start(args, title);
  DiagFix *fix = make_fix(line, column, end_line, end_column, text, title, args);
  va_end(args);
  fix_free(list->items[index].fix);
  list->items[index].fix = fix;
}

void diag_set_removal(
  DiagList *list,
  size_t index,
  int line,
  int column,
  int end_line,
  int end_column,
  const char *title,
  ...
) {
  if (index >= list->count) {
    return;
  }
  va_list args;
  va_start(args, title);
  DiagFix *fix = make_fix(line, column, end_line, end_column, "", title, args);
  va_end(args);
  if (fix != NULL) {
    fix->whole_lines = true;
  }
  fix_free(list->items[index].fix);
  list->items[index].fix = fix;
}

void diag_drop_fixes(DiagList *list, int check) {
  for (size_t i = 0; i < list->count; ++i) {
    if (list->items[i].check == check) {
      fix_free(list->items[i].fix);
      list->items[i].fix = NULL;
    }
  }
}

/* Whether line `line` of `source` holds nothing but blanks -- or does not exist. */
static bool blank_line(const char *source, int line) {
  size_t at = line > 0 ? offset_of(source, line, 1) : SIZE_MAX;
  if (at == SIZE_MAX) {
    return true;
  }
  while (source[at] == ' ' || source[at] == '\t' || source[at] == '\r') {
    at += 1;
  }
  return source[at] == '\n' || source[at] == '\0';
}

static int line_count(const char *source) {
  int lines = 1;
  for (const char *at = source; *at != '\0'; ++at) {
    lines += *at == '\n' && at[1] != '\0';
  }
  return lines;
}

void diag_settle_fixes(DiagList *list, const char *source) {
  for (size_t i = 0; i < list->count; ++i) {
    DiagFix *fix = list->items[i].fix;
    if (fix == NULL || !fix->whole_lines || list->items[i].file != NULL) {
      continue;
    }
    fix->whole_lines = false;

    size_t start = offset_of(source, fix->line, fix->column);
    size_t end = offset_of(source, fix->end_line, fix->end_column);
    if (start == SIZE_MAX) {
      continue;
    }
    if (end == SIZE_MAX) {
      end = strlen(source);
    }

    /* What else is on the first and last lines. Only blanks before, and only blanks or a comment
     * after, and the declaration has those lines to itself. */
    size_t line_start = start;
    while (line_start > 0 && source[line_start - 1] != '\n') {
      line_start -= 1;
    }
    bool alone_before = true;
    for (size_t at = line_start; at < start; ++at) {
      alone_before = alone_before && (source[at] == ' ' || source[at] == '\t');
    }
    size_t after = end;
    while (source[after] == ' ' || source[after] == '\t') {
      after += 1;
    }
    bool alone_after = source[after] == '\n' || source[after] == '\0' || source[after] == '\r' || source[after] == '#';

    if (!alone_before || !alone_after) {
      /* Shares a line with something that stays: take the blanks after it, so what follows closes up. */
      int end_line = fix->end_line;
      int end_column = fix->end_column + (int)(after - end);
      if (source[after] == '\n' || source[after] == '\0' || source[after] == '#') {
        end_column = fix->end_column;
      }
      fix->end_line = end_line;
      fix->end_column = end_column;
      continue;
    }

    int first = fix->line;
    int last = fix->end_line;
    /* One blank line goes with it, so two declarations it stood between keep exactly one between them:
     * the one after when there is one, or else the one before. At the top of the file there is nothing
     * before it to keep apart, so the blank line after goes too. */
    if (blank_line(source, first - 1) && blank_line(source, last + 1) && last < line_count(source)) {
      last += 1;
    } else if (last >= line_count(source) && first > 1 && blank_line(source, first - 1)) {
      first -= 1;
    }
    fix->line = first;
    fix->column = 1;
    fix->end_line = last + 1;
    fix->end_column = 1;
  }
}

/* Edits between two words, a swap of neighbours counting as one: optimal string alignment distance.
 * Bounded by the caller to words a name could plausibly be, so the fixed table is enough. */
static size_t distance(const char *a, size_t a_length, const char *b, size_t b_length) {
  enum { MAX = 64 };
  if (a_length >= MAX || b_length >= MAX) {
    return SIZE_MAX;
  }
  size_t table[MAX + 1][MAX + 1];
  for (size_t i = 0; i <= a_length; ++i) {
    table[i][0] = i;
  }
  for (size_t j = 0; j <= b_length; ++j) {
    table[0][j] = j;
  }
  for (size_t i = 1; i <= a_length; ++i) {
    for (size_t j = 1; j <= b_length; ++j) {
      size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
      size_t best = table[i - 1][j] + 1;
      best = table[i][j - 1] + 1 < best ? table[i][j - 1] + 1 : best;
      best = table[i - 1][j - 1] + cost < best ? table[i - 1][j - 1] + cost : best;
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && table[i - 2][j - 2] + 1 < best) {
        best = table[i - 2][j - 2] + 1;
      }
      table[i][j] = best;
    }
  }
  return table[a_length][b_length];
}

const char *diag_closest(const char *word, size_t length, const char *const *candidates, size_t count) {
  const char *best = NULL;
  size_t best_distance = SIZE_MAX;
  bool tied = false;
  for (size_t i = 0; i < count; ++i) {
    if (candidates[i] == NULL) {
      continue;
    }
    size_t d = distance(word, length, candidates[i], strlen(candidates[i]));
    if (d == 0 || d > 2 || d >= length) {
      continue;
    }
    if (d < best_distance) {
      best = candidates[i];
      best_distance = d;
      tied = false;
    } else if (d == best_distance && strcmp(candidates[i], best) != 0) {
      tied = true;
    }
  }
  return tied ? NULL : best;
}
