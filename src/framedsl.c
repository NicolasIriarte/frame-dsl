/* realpath() is POSIX, not C11. */
#define _XOPEN_SOURCE 700

#include "framedsl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compose.h"
#include "definition.h"
#include "diag.h"
#include "import.h"
#include "lint.h"
#include "parser.h"

struct FramedslResult {
  FramedslStatus status;
  char *name;

  /* Owned. Kept whether the load succeeded or not: a successful load can still be interesting to
   * report on, and an empty list is a simpler contract than a sometimes-absent one. */
  DiagList diags;
  /* Parallel to diags, in the public shape. Built once at the end so the pointers handed out stay
   * valid for the life of the result. */
  FramedslDiagnostic *public_diags;

  FramedslDefinition definition;
  bool have_definition;
};

static FramedslResult *result_new(const char *name) {
  FramedslResult *result = calloc(1, sizeof(*result));
  if (result == NULL) {
    return NULL;
  }
  diag_init(&result->diags);

  const char *label = name != NULL ? name : "<input>";
  result->name = malloc(strlen(label) + 1);
  if (result->name == NULL) {
    free(result);
    return NULL;
  }
  strcpy(result->name, label);
  return result;
}

/* Built after every pass has run, so no reallocation can move what a caller is holding. */
static bool publish_diagnostics(FramedslResult *result) {
  if (result->diags.count == 0) {
    return true;
  }

  result->public_diags = calloc(result->diags.count, sizeof(*result->public_diags));
  if (result->public_diags == NULL) {
    return false;
  }

  for (size_t i = 0; i < result->diags.count; ++i) {
    result->public_diags[i] = (FramedslDiagnostic){
      .line = result->diags.items[i].line,
      .column = result->diags.items[i].column,
      .check = result->diags.items[i].check,
      .severity = result->diags.items[i].severity == DIAG_WARNING ? FRAMEDSL_SEVERITY_WARNING : FRAMEDSL_SEVERITY_ERROR,
      .end_line = result->diags.items[i].end_line,
      .end_column = result->diags.items[i].end_column,
      .message = result->diags.items[i].message,
      .file = result->diags.items[i].file,
    };
  }
  return true;
}

/* The whole load. `base_dir` is where imports are resolved from, and `self` the real path of the file
 * being loaded, or NULL for a string. */
static FramedslResult *load(const char *source, const char *name, const char *base_dir, const char *self) {
  FramedslResult *result = result_new(name);
  if (result == NULL) {
    return NULL;
  }
  if (source == NULL) {
    result->status = FRAMEDSL_ERROR_IO;
    return result;
  }

  AstFile *ast = parse(source, &result->diags);
  if (ast == NULL) {
    result->status = FRAMEDSL_ERROR_MEMORY;
    diag_fill_spans(&result->diags, source);
    publish_diagnostics(result);
    return result;
  }

  /* The checks only run on a tree that parsed. Over a partial one they report consequences of the
   * syntax error as though they were separate mistakes. */
  Layout *layout = NULL;
  if (diag_error_count(&result->diags) == 0 && imports_resolve(ast, base_dir, self, &result->diags)) {
    /* Before composition, which is the end of anything naming a block. Only the file being loaded is
     * linted: an imported file's warnings belong to whoever edits it. */
    lint_unused(ast, &result->diags);
    if (compose_file(ast, &result->diags)) {
      layout = check_file(ast, &result->diags);
    }
  }

  /* Codegen runs only on a file that passed every check, so what it can still fail on is an
   * allocation or an expression nested deeper than the evaluator's stack -- not a mistake the checks
   * were supposed to have caught. */
  Program *program = NULL;
  CalTable *cals = NULL;
  if (layout != NULL) {
    program = program_compile(ast, layout, &result->diags);
  }
  if (program != NULL) {
    cals = cal_compile(ast, &result->diags);
  }

  if (layout == NULL || program == NULL || cals == NULL) {
    cal_table_free(cals);
    program_free(program);
    layout_free(layout);
    ast_free(ast);
    result->status = FRAMEDSL_ERROR_INVALID;
  } else {
    result->definition.ast = ast;
    result->definition.layout = layout;
    result->definition.program = program;
    result->definition.cals = cals;
    result->definition.tag = definition_next_tag();
    result->have_definition = true;
    result->status = FRAMEDSL_OK;
  }

  diag_fill_spans(&result->diags, source);
  if (!publish_diagnostics(result)) {
    result->status = FRAMEDSL_ERROR_MEMORY;
  }
  return result;
}

FramedslResult *framedsl_load_string(const char *source, const char *name) {
  /* Nothing to be relative to but where the program is running. */
  return load(source, name, ".", NULL);
}

/* Definition files describe a handful of layouts, so there is no reason to stream one. */
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

/* Loads `source` as the file at `path` would be: attributed to it, and importing from beside it. */
static FramedslResult *load_as(const char *source, const char *path) {
  /* Imports are relative to the file doing the importing, so a tree of definition files moves as a
   * unit. */
  char directory[PATH_MAX];
  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    snprintf(directory, sizeof(directory), ".");
  } else {
    snprintf(directory, sizeof(directory), "%.*s", slash == path ? 1 : (int)(slash - path), path);
  }
  /* The real path is what an import cycle back to this file is recognised by. A buffer an editor has
   * not saved yet may have no file behind it at all, and then there is nothing for a cycle to reach. */
  char self[PATH_MAX];
  bool resolved = realpath(path, self) != NULL;

  return load(source, path, directory, resolved ? self : NULL);
}

FramedslResult *framedsl_load_file(const char *path) {
  if (path == NULL) {
    return framedsl_load_string(NULL, NULL);
  }

  char *source = read_whole_file(path);
  if (source == NULL) {
    FramedslResult *result = result_new(path);
    if (result == NULL) {
      return NULL;
    }
    result->status = FRAMEDSL_ERROR_IO;
    diag_add(&result->diags, 0, 0, 0, "cannot read '%s'", path);
    publish_diagnostics(result);
    return result;
  }

  FramedslResult *result = load_as(source, path);
  free(source);
  return result;
}

FramedslResult *framedsl_load_source(const char *source, const char *path) {
  if (path == NULL) {
    return framedsl_load_string(source, NULL);
  }
  if (source == NULL) {
    return framedsl_load_string(NULL, path);
  }
  return load_as(source, path);
}

void framedsl_result_free(FramedslResult *result) {
  if (result == NULL) {
    return;
  }
  if (result->have_definition) {
    program_free(result->definition.program);
    cal_table_free(result->definition.cals);
    layout_free(result->definition.layout);
    ast_free(result->definition.ast);
  }
  free(result->public_diags);
  diag_free(&result->diags);
  free(result->name);
  free(result);
}

FramedslStatus framedsl_status(const FramedslResult *result) {
  return result == NULL ? FRAMEDSL_ERROR_MEMORY : result->status;
}

bool framedsl_ok(const FramedslResult *result) {
  return result != NULL && result->status == FRAMEDSL_OK;
}

const char *framedsl_source_name(const FramedslResult *result) {
  return result == NULL ? "<input>" : result->name;
}

size_t framedsl_diagnostic_count(const FramedslResult *result) {
  return result == NULL ? 0 : result->diags.count;
}

const FramedslDiagnostic *framedsl_diagnostic(const FramedslResult *result, size_t index) {
  if (result == NULL || index >= result->diags.count || result->public_diags == NULL) {
    return NULL;
  }
  return &result->public_diags[index];
}

const FramedslDefinition *framedsl_definition(const FramedslResult *result) {
  if (result == NULL || !result->have_definition) {
    return NULL;
  }
  return &result->definition;
}

size_t framedsl_packet_count(const FramedslDefinition *definition) {
  return definition == NULL ? 0 : definition->ast->packet_count;
}

const char *framedsl_packet_name(const FramedslDefinition *definition, size_t index) {
  if (definition == NULL || index >= definition->ast->packet_count) {
    return NULL;
  }
  return definition->ast->packets[index].name;
}

uint64_t framedsl_packet_reserved_bits(const FramedslDefinition *definition, size_t index) {
  if (definition == NULL || index >= definition->layout->packet_count) {
    return 0;
  }
  return definition->layout->packets[index].reserved_bits;
}

bool framedsl_find_packet(const FramedslDefinition *definition, const char *name, size_t *index) {
  if (definition == NULL || name == NULL) {
    return false;
  }
  for (size_t i = 0; i < definition->ast->packet_count; ++i) {
    if (strcmp(definition->ast->packets[i].name, name) == 0) {
      if (index != NULL) {
        *index = i;
      }
      return true;
    }
  }
  return false;
}

size_t framedsl_field_count(const FramedslDefinition *definition, size_t packet) {
  if (definition == NULL || packet >= definition->ast->packet_count) {
    return 0;
  }
  return definition->ast->packets[packet].field_count;
}

bool framedsl_field(const FramedslDefinition *definition, size_t packet, size_t field, FramedslField *out) {
  if (definition == NULL || out == NULL || packet >= definition->ast->packet_count) {
    return false;
  }
  const AstPacket *ast_packet = &definition->ast->packets[packet];
  if (field >= ast_packet->field_count) {
    return false;
  }

  const AstField *ast_field = &ast_packet->fields[field];
  const FieldLayout *slot = &definition->layout->packets[packet].fields[field];

  *out = (FramedslField){
    .name = ast_field->name,
    .variable = slot->variable,
    .width_bits = slot->variable ? 0 : slot->width_bits,
    .max_bits = slot->variable ? slot->max_bits : 0,
    .reserved_bits = slot->reserved_bits,
    .offset_known = slot->offset_known,
    .offset_bits = slot->offset_known ? slot->offset_bits : 0,
    .is_scalar = slot->is_scalar,
    .has_fixed = ast_field->has_fixed,
    .fixed = ast_field->has_fixed ? ast_field->fixed : 0,
    .bit_order =
      ast_field->bit_order == BIT_ORDER_LSB_FIRST ? FRAMEDSL_BIT_ORDER_LSB_FIRST : FRAMEDSL_BIT_ORDER_MSB_FIRST,
    .byte_order = ast_field->byte_order == BYTE_ORDER_LITTLE_ENDIAN ? FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN
                                                                    : FRAMEDSL_BYTE_ORDER_BIG_ENDIAN,
  };

  const int32_t cal = definition->program->packets[packet].fields[field].cal;
  if (cal >= 0) {
    out->cal = definition->cals->cals[cal].name;
    out->cal_invertible = definition->cals->cals[cal].invertible;
  }
  return true;
}

bool framedsl_find_field(const FramedslDefinition *definition, size_t packet, const char *name, size_t *index) {
  if (definition == NULL || name == NULL || packet >= definition->ast->packet_count) {
    return false;
  }
  const AstPacket *ast_packet = &definition->ast->packets[packet];
  for (size_t i = 0; i < ast_packet->field_count; ++i) {
    if (strcmp(ast_packet->fields[i].name, name) == 0) {
      if (index != NULL) {
        *index = i;
      }
      return true;
    }
  }
  return false;
}

/* ---- Calibrations ---------------------------------------------------------------------------- */

size_t framedsl_cal_count(const FramedslDefinition *definition) {
  return definition == NULL ? 0 : definition->cals->count;
}

const char *framedsl_cal_name(const FramedslDefinition *definition, size_t index) {
  if (definition == NULL || index >= definition->cals->count) {
    return NULL;
  }
  return definition->cals->cals[index].name;
}

bool framedsl_find_cal(const FramedslDefinition *definition, const char *name, FramedslCalId *out) {
  if (definition == NULL || name == NULL) {
    return false;
  }
  for (size_t i = 0; i < definition->cals->count; ++i) {
    if (strcmp(definition->cals->cals[i].name, name) == 0) {
      if (out != NULL) {
        *out = (FramedslCalId){ .owner = definition->tag, .index = (uint32_t)i };
      }
      return true;
    }
  }
  return false;
}

static const CalCompiled *resolve_cal(const FramedslDefinition *definition, FramedslCalId cal) {
  if (definition == NULL || cal.owner != definition->tag || cal.index >= definition->cals->count) {
    return NULL;
  }
  return &definition->cals->cals[cal.index];
}

FramedslStatus framedsl_calibrate(const FramedslDefinition *definition, FramedslCalId cal, double raw, double *eng) {
  const CalCompiled *compiled = resolve_cal(definition, cal);
  if (compiled == NULL || eng == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  return cal_forward(compiled, raw, eng) ? FRAMEDSL_OK : FRAMEDSL_ERROR_RANGE;
}

FramedslStatus framedsl_uncalibrate(const FramedslDefinition *definition, FramedslCalId cal, double eng, double *raw) {
  const CalCompiled *compiled = resolve_cal(definition, cal);
  if (compiled == NULL || raw == NULL || !compiled->invertible) {
    return FRAMEDSL_ERROR_FIELD;
  }
  return cal_backward(compiled, eng, raw) ? FRAMEDSL_OK : FRAMEDSL_ERROR_RANGE;
}

size_t framedsl_emit_c(const FramedslDefinition *definition, const char *prefix, char *out, size_t capacity) {
  if (definition == NULL) {
    if (out != NULL && capacity > 0) {
      out[0] = '\0';
    }
    return 0;
  }
  return cal_emit_c(definition->cals, prefix == NULL ? "fdsl_" : prefix, out, capacity);
}
