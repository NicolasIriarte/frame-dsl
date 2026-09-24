#include "lint.h"

#include <stdbool.h>
#include <string.h>

/* Every name something declared in this file refers to. Declarations merged from an import carry an
 * origin and are left out: they refer to what their own file declared, and that is no evidence that
 * anything *here* is used. */
typedef struct {
  const AstFile *file;
} Uses;

static bool cal_expr_calls(const CalExpr *expr, const char *name) {
  if (expr == NULL) {
    return false;
  }
  if (expr->kind == CAL_EXPR_CALL && expr->name != NULL && strcmp(expr->name, name) == 0) {
    return true;
  }
  return cal_expr_calls(expr->lhs, name) || cal_expr_calls(expr->rhs, name);
}

static bool fields_use(const AstPacket *container, const char *name) {
  for (size_t f = 0; f < container->field_count; ++f) {
    const AstField *field = &container->fields[f];
    if ((field->block != NULL && strcmp(field->block, name) == 0)
        || (field->cal != NULL && strcmp(field->cal, name) == 0)) {
      return true;
    }
  }
  return false;
}

static bool is_used(const Uses *uses, const char *name) {
  const AstFile *file = uses->file;
  for (size_t p = 0; p < file->packet_count; ++p) {
    if (fields_use(&file->packets[p], name)) {
      return true;
    }
  }
  for (size_t b = 0; b < file->block_count; ++b) {
    if (file->blocks[b].origin == NULL && fields_use(&file->blocks[b], name)) {
      return true;
    }
  }
  for (size_t c = 0; c < file->cal_count; ++c) {
    const AstCal *cal = &file->cals[c];
    if (cal->origin == NULL && (cal_expr_calls(cal->body, name) || cal_expr_calls(cal->inverse, name))) {
      return true;
    }
  }
  return false;
}

void lint_unused(const AstFile *file, DiagList *diags) {
  if (file->packet_count == 0) {
    return;
  }
  const Uses uses = { .file = file };

  for (size_t b = 0; b < file->block_count; ++b) {
    const AstPacket *block = &file->blocks[b];
    if (block->origin == NULL && block->name != NULL && !is_used(&uses, block->name)) {
      size_t index = diags->count;
      diag_warn(diags, block->name_line, block->name_column, 30, "block '%s' is not used by anything", block->name);
      /* From its attribute group, if it has one, to just past the `;` after its closing brace. */
      diag_set_removal(
        diags,
        index,
        block->start_line,
        block->start_column,
        block->end_line,
        block->end_column + 2,
        "remove the unused block '%s'",
        block->name
      );
    }
  }

  for (size_t c = 0; c < file->cal_count; ++c) {
    const AstCal *cal = &file->cals[c];
    if (cal->origin == NULL && cal->name != NULL && !is_used(&uses, cal->name)) {
      size_t index = diags->count;
      diag_warn(diags, cal->name_line, cal->name_column, 30, "calibration '%s' is not used by anything", cal->name);
      if (cal->end_line > 0) {
        diag_set_removal(
          diags,
          index,
          cal->line,
          cal->column,
          cal->end_line,
          cal->end_column,
          "remove the unused calibration '%s'",
          cal->name
        );
      }
    }
  }

  for (const AstImport *import = file->imports; import != NULL; import = import->next) {
    bool used = false;
    for (size_t n = 0; n < import->brought_count && !used; ++n) {
      used = is_used(&uses, import->brought[n]);
    }
    if (!used) {
      size_t index = diags->count;
      diag_warn(
        diags,
        import->line,
        import->column,
        30,
        "nothing '%s' brings in is used%s",
        import->path,
        import->brought_count == 0 ? " -- it declares no blocks or calibrations" : ""
      );
      if (import->end_line > 0) {
        diag_set_removal(
          diags, index, import->line, import->column, import->end_line, import->end_column, "remove the unused import"
        );
      }
    }
  }
}
