#pragma once

/**
 * @file
 * @brief Collected diagnostics.
 *
 * A load reports everything it found, not the first thing. Stopping at the first error means a file
 * with four mistakes takes four runs to fix, and the parser already knows how to keep going.
 */

#include <stdbool.h>
#include <stddef.h>

/** @brief Past this many, the file is being reported on rather than read, and the rest is noise. */
#define DIAG_MAX 50

typedef struct {
  int line;
  int column;
  /**
   * @brief The numbered check from docs/design.md this comes from, or 0 for a syntax error.
   *
   * Carried so a caller -- a test especially -- can assert a file was rejected for the right reason
   * rather than for any reason at all.
   */
  int check;
  char *message;
} Diag;

typedef struct {
  Diag *items;
  size_t count;
  size_t capacity;
  /** @brief Set once DIAG_MAX is hit, so a caller can say "and more" rather than imply that was all. */
  bool overflowed;
} DiagList;

void diag_init(DiagList *list);
void diag_free(DiagList *list);

/**
 * @brief Add a diagnostic, printf-style. Silently drops past DIAG_MAX, recording that it did.
 * @param list The list to add to.
 * @param line Where the problem is.
 * @param column Where the problem is.
 * @param check The numbered check this comes from, or 0 for a syntax error.
 * @param fmt printf-style format string, and the arguments for it.
 */
void diag_add(DiagList *list, int line, int column, int check, const char *fmt, ...)
  __attribute__((format(printf, 5, 6)));

/** @brief Sorted by position, so output reads in file order however the passes found things. */
void diag_sort(DiagList *list);
