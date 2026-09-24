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

/**
 * @brief How bad it is. An error fails the load; a warning is reported and the load goes ahead.
 */
typedef enum {
  DIAG_ERROR,
  DIAG_WARNING,
} DiagSeverity;

/**
 * @brief An edit that would fix a diagnostic: replace `line`:`column` up to `end_line`:`end_column`
 * with `text` -- an insertion when the two are the same place, a deletion when `text` is empty.
 *
 * Made by whatever reported the problem, since that is what knows what was meant; everything after it
 * only relays the fix. Positions are the compiler's: lines and byte columns from 1.
 */
typedef struct {
  int line;
  int column;
  int end_line;
  int end_column;
  char *text;
  /** @brief What applying it does, said the way a menu entry says it: "change to 'byte_order'". */
  char *title;
  /**
   * @brief Set for a removal of whole declarations: diag_settle_fixes() widens it to the whole lines
   * it stands on, and one of the blank lines either side, once there is source to look at.
   */
  bool whole_lines;
} DiagFix;

typedef struct {
  int line;
  int column;
  /**
   * @brief Just past the last character the problem covers, so an editor underlines the name and not
   * one character of it. Zero until diag_fill_spans() has looked at the source; the start again when
   * there is nothing there to cover.
   */
  int end_line;
  int end_column;
  DiagSeverity severity;
  /**
   * @brief The numbered check from docs/design.md this comes from, or 0 for a syntax error.
   *
   * Carried so a caller -- a test especially -- can assert a file was rejected for the right reason
   * rather than for any reason at all.
   */
  int check;
  char *message;
  /**
   * @brief The imported file this is in, or NULL for the file being loaded. Owned.
   *
   * An imported file is loaded and checked on its own, and its diagnostics are carried into the
   * importing file's list with this set -- so a position is always a position in the file named.
   */
  char *file;
  /** @brief How to fix it, when there is one obvious way; NULL otherwise. Owned. */
  DiagFix *fix;
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

/** @brief As diag_add(), for something that is reported but does not fail the load. */
void diag_warn(DiagList *list, int line, int column, int check, const char *fmt, ...)
  __attribute__((format(printf, 5, 6)));

/**
 * @brief How many of the list are errors. What decides whether a load failed: counting everything
 * would make a warning fatal.
 */
size_t diag_error_count(const DiagList *list);

/** @brief Removes every warning, keeping the errors in order. */
void diag_drop_warnings(DiagList *list);

/**
 * @brief Gives every diagnostic of this file a span: the token that starts where it does.
 *
 * Positions are all a pass has at hand when it reports, and the token there is nearly always the
 * thing being complained about -- a name, a number, the unexpected token -- so the span is found
 * afterwards, once, rather than threaded through fifty call sites. Diagnostics attributed to an
 * imported file are skipped: their positions are in a source this is not.
 * @param list The list to fill in.
 * @param source The text the list's positions are in.
 */
void diag_fill_spans(DiagList *list, const char *source);

/** @brief Sorted by position, so output reads in file order however the passes found things. */
void diag_sort(DiagList *list);

/**
 * @brief Copies every diagnostic of `from` onto `to`, attributing those without a file to `file`.
 *
 * One already attributed keeps its own: it came from a file imported by `file`, and that is where its
 * position is.
 */
void diag_append(DiagList *to, const DiagList *from, const char *file);

/**
 * @brief Attach a fix to the diagnostic at `index`, typically the one just added -- nothing happens if
 * that one was not added after all (the list was full, or memory ran out).
 * @param text The replacement; "" to delete.
 * @param title printf-style: what the fix does, for a menu and for `--check`'s note.
 */
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
) __attribute__((format(printf, 8, 9)));

/** @brief As diag_set_fix(), and marks the fix as a removal of whole lines; see DiagFix.whole_lines. */
void diag_set_removal(
  DiagList *list,
  size_t index,
  int line,
  int column,
  int end_line,
  int end_column,
  const char *title,
  ...
) __attribute__((format(printf, 7, 8)));

/** @brief Takes the fixes off every diagnostic of check `check`, keeping the diagnostics. */
void diag_drop_fixes(DiagList *list, int check);

/** @brief Widens every whole-line removal to the lines it stands on, now that `source` can be read. */
void diag_settle_fixes(DiagList *list, const char *source);

/**
 * @brief The one candidate closest to `word`, for a "did you mean": within two edits (an insertion, a
 * deletion, a substitution or two neighbours swapped), and fewer edits than `word` has letters.
 * @return NULL when nothing is that close, or when two candidates are equally close -- a guess
 * between them would be a coin toss offered as a fix.
 */
const char *diag_closest(const char *word, size_t length, const char *const *candidates, size_t count);
