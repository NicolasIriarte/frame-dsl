#include "framedsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "definition.h"
#include "diag.h"
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
        .message = result->diags.items[i].message,
    };
  }
  return true;
}

FramedslResult *framedsl_load_string(const char *source, const char *name) {
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
    publish_diagnostics(result);
    return result;
  }

  /* The checks only run on a tree that parsed. Over a partial one they report consequences of the
   * syntax error as though they were separate mistakes. */
  Layout *layout = NULL;
  if (result->diags.count == 0) {
    layout = check_file(ast, &result->diags);
  }

  if (layout == NULL) {
    ast_free(ast);
    result->status = FRAMEDSL_ERROR_INVALID;
  } else {
    result->definition.ast = ast;
    result->definition.layout = layout;
    result->definition.tag = definition_next_tag();
    result->have_definition = true;
    result->status = FRAMEDSL_OK;
  }

  if (!publish_diagnostics(result)) {
    result->status = FRAMEDSL_ERROR_MEMORY;
  }
  return result;
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

  FramedslResult *result = framedsl_load_string(source, path);
  free(source);
  return result;
}

void framedsl_result_free(FramedslResult *result) {
  if (result == NULL) {
    return;
  }
  if (result->have_definition) {
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

bool framedsl_field(const FramedslDefinition *definition, size_t packet, size_t field,
                    FramedslField *out) {
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
      .bit_order = ast_field->bit_order == BIT_ORDER_LSB_FIRST ? FRAMEDSL_BIT_ORDER_LSB_FIRST
                                                               : FRAMEDSL_BIT_ORDER_MSB_FIRST,
      .byte_order = ast_field->byte_order == BYTE_ORDER_LITTLE_ENDIAN
                        ? FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN
                        : FRAMEDSL_BYTE_ORDER_BIG_ENDIAN,
  };
  return true;
}

bool framedsl_find_field(const FramedslDefinition *definition, size_t packet, const char *name,
                         size_t *index) {
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
