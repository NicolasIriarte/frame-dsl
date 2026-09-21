/* realpath() is POSIX, not C11. */
#define _XOPEN_SOURCE 700

#include "import.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compose.h"
#include "parser.h"

/* Deeper than any real tree of definition files, and shallow enough to stop a runaway that the cycle
 * check somehow missed -- two paths to one file that realpath() cannot tell apart, say. */
#define IMPORT_DEPTH_MAX 32

typedef struct {
  /* The real path of every file currently being imported, outermost first: what a cycle would come
   * back to. */
  const char *stack[IMPORT_DEPTH_MAX + 1];
  size_t depth;
} Chain;

static char *read_whole_file(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    return NULL;
  }
  if (fseek(handle, 0, SEEK_END) != 0) {
    fclose(handle);
    return NULL;
  }
  long size = ftell(handle);
  if (size < 0) {
    fclose(handle);
    return NULL;
  }
  rewind(handle);

  char *source = malloc((size_t)size + 1);
  if (source == NULL) {
    fclose(handle);
    return NULL;
  }
  size_t read = fread(source, 1, (size_t)size, handle);
  fclose(handle);
  source[read] = '\0';
  return source;
}

/* `text` in `file`'s region, so it lives as long as the tree it is put into. */
static const char *keep(AstFile *file, const char *text) {
  return region_strndup(file->region, text, strlen(text));
}

static const char *prefixed(AstFile *file, const char *prefix, const char *name) {
  if (prefix == NULL || name == NULL) {
    return name;
  }
  char joined[512];
  snprintf(joined, sizeof(joined), "%s.%s", prefix, name);
  return keep(file, joined);
}

/* A calibration body copied for an `as` import: every call is to a name of the imported file, so
 * every call gets the prefix, and the body keeps calling the curves it called there. */
static CalExpr *rename_calls(AstFile *file, const CalExpr *expr, const char *prefix) {
  if (expr == NULL || prefix == NULL) {
    return (CalExpr *)expr;
  }
  CalExpr *copy = region_alloc(file->region, sizeof(CalExpr));
  if (copy == NULL) {
    return (CalExpr *)expr;
  }
  *copy = *expr;
  if (expr->kind == CAL_EXPR_CALL) {
    copy->name = prefixed(file, prefix, expr->name);
  }
  copy->lhs = rename_calls(file, expr->lhs, prefix);
  copy->rhs = rename_calls(file, expr->rhs, prefix);
  return copy;
}

static bool keep_imported(AstFile *file, AstFile *imported) {
  if (file->imported_count == file->imported_capacity) {
    size_t capacity = file->imported_capacity == 0 ? 4 : file->imported_capacity * 2;
    AstFile **grown = realloc(file->imported, capacity * sizeof(*grown));
    if (grown == NULL) {
      return false;
    }
    file->imported = grown;
    file->imported_capacity = capacity;
  }
  file->imported[file->imported_count++] = imported;
  return true;
}

/* What an import merges, gathered before any of it is put in front of the file's own declarations. */
typedef struct {
  AstPacket *blocks;
  size_t block_count;
  size_t block_capacity;
  AstCal *cals;
  size_t cal_count;
  size_t cal_capacity;
} Merged;

static bool grow(void **items, size_t *capacity, size_t count, size_t size) {
  if (count < *capacity) {
    return true;
  }
  size_t next = *capacity == 0 ? 8 : *capacity * 2;
  void *grown = realloc(*items, next * size);
  if (grown == NULL) {
    return false;
  }
  *items = grown;
  *capacity = next;
  return true;
}

/* Check 28, between one declaration about to be merged and one already merged. The same declaration
 * reached twice -- a diamond -- is not a clash: that is what `origin` records. Returns whether the new
 * one should still be added. */
static bool admit(
  const char *kind,
  const char *name,
  const char *origin,
  const char *other_name,
  const char *other_origin,
  const AstImport *import,
  DiagList *diags,
  bool *failed
) {
  if (strcmp(name, other_name) != 0) {
    return true;
  }
  if (strcmp(origin, other_origin) != 0) {
    diag_add(
      diags,
      import->line,
      import->column,
      28,
      "importing '%s' brings a %s '%s' from '%s', and one of that name already came from '%s'; import "
      "one of them with 'as' to keep both",
      import->path,
      kind,
      name,
      origin,
      other_origin
    );
    *failed = true;
  }
  return false;
}

static bool merge(
  AstFile *file,
  AstFile *imported,
  const char *real,
  const AstImport *import,
  Merged *merged,
  DiagList *diags
) {
  bool failed = false;
  const char *prefix = import->alias;

  for (size_t b = 0; b < imported->block_count; ++b) {
    const AstPacket *block = &imported->blocks[b];
    const char *name = prefixed(file, prefix, block->name);
    const char *origin = block->origin != NULL ? block->origin : keep(file, real);

    bool admitted = true;
    for (size_t i = 0; i < merged->block_count && admitted; ++i) {
      admitted = admit("block", name, origin, merged->blocks[i].name, merged->blocks[i].origin, import, diags, &failed);
    }
    if (!admitted) {
      continue;
    }

    if (!grow((void **)&merged->blocks, &merged->block_capacity, merged->block_count, sizeof(AstPacket))) {
      return false;
    }
    AstPacket *copy = &merged->blocks[merged->block_count++];
    *copy = *block;
    copy->name = name;
    copy->origin = origin;
    /* Declared from the top: an import stands in front of everything in the file. */
    copy->end_line = 0;
    copy->end_column = 0;

    /* The field array is the file's to free, so it gets its own; the fields point into the imported
     * tree, which the file keeps. A calibration a field names came from the same file and gets the
     * same prefix. */
    copy->fields = malloc((block->field_count == 0 ? 1 : block->field_count) * sizeof(AstField));
    if (copy->fields == NULL) {
      merged->block_count -= 1;
      return false;
    }
    memcpy(copy->fields, block->fields, block->field_count * sizeof(AstField));
    copy->field_capacity = block->field_count;
    for (size_t f = 0; f < copy->field_count; ++f) {
      copy->fields[f].cal = prefixed(file, prefix, copy->fields[f].cal);
    }
  }

  for (size_t c = 0; c < imported->cal_count; ++c) {
    const AstCal *cal = &imported->cals[c];
    const char *name = prefixed(file, prefix, cal->name);
    const char *origin = cal->origin != NULL ? cal->origin : keep(file, real);

    bool admitted = true;
    for (size_t i = 0; i < merged->cal_count && admitted; ++i) {
      admitted =
        admit("calibration", name, origin, merged->cals[i].name, merged->cals[i].origin, import, diags, &failed);
    }
    if (!admitted) {
      continue;
    }

    if (!grow((void **)&merged->cals, &merged->cal_capacity, merged->cal_count, sizeof(AstCal))) {
      return false;
    }
    AstCal *copy = &merged->cals[merged->cal_count++];
    *copy = *cal;
    copy->name = name;
    copy->origin = origin;
    copy->body = rename_calls(file, cal->body, prefix);
    copy->inverse = rename_calls(file, cal->inverse, prefix);
  }

  return !failed;
}

/* The directory part of `path`, into `out`: "." when there is none. */
static void directory_of(const char *path, char *out, size_t capacity) {
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    snprintf(out, capacity, ".");
  } else if (slash == path) {
    snprintf(out, capacity, "/");
  } else {
    snprintf(out, capacity, "%.*s", (int)(slash - path), path);
  }
}

static bool resolve(AstFile *file, const char *base_dir, Chain *chain, DiagList *diags);

/* Reads, parses, resolves, composes and checks one imported file. Its problems go into `diags`
 * attributed to `shown`. NULL if it had any. */
static AstFile *load_imported(const char *shown, const char *real, Chain *chain, DiagList *diags) {
  char *source = read_whole_file(real);
  if (source == NULL) {
    return NULL;
  }

  DiagList own;
  diag_init(&own);
  AstFile *imported = parse(source, &own);
  free(source);

  char directory[PATH_MAX];
  directory_of(shown, directory, sizeof(directory));

  bool ok = imported != NULL && own.count == 0;
  if (ok) {
    chain->stack[chain->depth++] = real;
    ok = resolve(imported, directory, chain, &own);
    chain->depth -= 1;
  }
  ok = ok && compose_file(imported, &own);
  if (ok) {
    /* Checked whole, packets and all: a broken file is broken whether or not its packets are the part
     * being imported. */
    Layout *layout = check_file(imported, &own);
    ok = layout != NULL;
    layout_free(layout);
  }

  diag_append(diags, &own, shown);
  diag_free(&own);
  if (!ok) {
    ast_free(imported);
    return NULL;
  }
  return imported;
}

static bool resolve(AstFile *file, const char *base_dir, Chain *chain, DiagList *diags) {
  bool ok = true;
  Merged merged = { 0 };

  for (const AstImport *import = file->imports; import != NULL; import = import->next) {
    char shown[PATH_MAX];
    if (import->path[0] == '/' || strcmp(base_dir, ".") == 0) {
      snprintf(shown, sizeof(shown), "%s", import->path);
    } else {
      snprintf(shown, sizeof(shown), "%s/%s", base_dir, import->path);
    }

    /* Check 26. */
    char real[PATH_MAX];
    if (realpath(shown, real) == NULL) {
      diag_add(diags, import->line, import->column, 26, "cannot read '%s'", shown);
      ok = false;
      continue;
    }

    /* Check 27. */
    bool cycle = false;
    for (size_t i = 0; i < chain->depth; ++i) {
      cycle = cycle || strcmp(chain->stack[i], real) == 0;
    }
    if (cycle || chain->depth >= IMPORT_DEPTH_MAX) {
      diag_add(
        diags,
        import->line,
        import->column,
        27,
        cycle ? "importing '%s' leads back to a file already being imported; imports cannot form a cycle"
              : "importing '%s' nests imports more deeply than this build allows",
        shown
      );
      ok = false;
      continue;
    }

    size_t reported = diags->count;
    AstFile *imported = load_imported(shown, real, chain, diags);
    if (imported == NULL) {
      if (diags->count == reported) {
        diag_add(diags, import->line, import->column, 26, "cannot read '%s'", shown);
      }
      ok = false;
      continue;
    }
    if (!keep_imported(file, imported)) {
      ast_free(imported);
      diag_add(diags, import->line, import->column, 0, "out of memory");
      ok = false;
      continue;
    }

    ok = merge(file, imported, real, import, &merged, diags) && ok;
  }

  /* What the imports brought goes in front of the file's own declarations. A local declaration of an
   * imported name is then the later of the two, and checks 1 and 17 report it where it is written. */
  if (merged.block_count > 0) {
    AstPacket *blocks = malloc((merged.block_count + file->block_count) * sizeof(AstPacket));
    if (blocks == NULL) {
      ok = false;
    } else {
      memcpy(blocks, merged.blocks, merged.block_count * sizeof(AstPacket));
      if (file->block_count > 0) {
        memcpy(blocks + merged.block_count, file->blocks, file->block_count * sizeof(AstPacket));
      }
      free(file->blocks);
      file->blocks = blocks;
      file->block_count += merged.block_count;
      file->block_capacity = file->block_count;
    }
  }
  if (merged.cal_count > 0) {
    AstCal *cals = malloc((merged.cal_count + file->cal_count) * sizeof(AstCal));
    if (cals == NULL) {
      ok = false;
    } else {
      memcpy(cals, merged.cals, merged.cal_count * sizeof(AstCal));
      if (file->cal_count > 0) {
        memcpy(cals + merged.cal_count, file->cals, file->cal_count * sizeof(AstCal));
      }
      free(file->cals);
      file->cals = cals;
      file->cal_count += merged.cal_count;
      file->cal_capacity = file->cal_count;
    }
  }
  free(merged.blocks);
  free(merged.cals);

  if (!ok && diags->count == 0) {
    diag_add(diags, 1, 1, 0, "out of memory");
  }
  return ok;
}

bool imports_resolve(AstFile *file, const char *base_dir, const char *self, DiagList *diags) {
  if (file->imports == NULL) {
    return true;
  }
  Chain chain = { .depth = 0 };
  if (self != NULL) {
    chain.stack[chain.depth++] = self;
  }
  bool ok = resolve(file, base_dir, &chain, diags);
  diag_sort(diags);
  return ok;
}
