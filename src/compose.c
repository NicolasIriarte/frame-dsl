#include "compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* More fields than any frame has, used to stop a file that nests blocks two-by-two a dozen deep from
 * expanding into millions. */
#define COMPOSE_FIELD_MAX 65536

typedef struct {
  AstFile *file;
  DiagList *diags;
  bool failed;
} Composer;

typedef struct {
  AstField *items;
  size_t count;
  size_t capacity;
} FieldList;

static void fail(Composer *composer, int line, int column, int check, const char *message) {
  composer->failed = true;
  diag_add(composer->diags, line, column, check, "%s", message);
}

static const AstPacket *find_block(const AstFile *file, const char *name) {
  for (size_t i = 0; i < file->block_count; ++i) {
    if (file->blocks[i].name != NULL && strcmp(file->blocks[i].name, name) == 0) {
      return &file->blocks[i];
    }
  }
  return NULL;
}

/* `prefix` + `name`, in the file's region so it lives as long as the tree. */
static const char *join(Composer *composer, const char *prefix, const char *name) {
  if (prefix[0] == '\0') {
    return name;
  }
  size_t length = strlen(prefix) + strlen(name);
  char *joined = region_alloc(composer->file->region, length + 1);
  if (joined == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return name;
  }
  strcpy(joined, prefix);
  strcat(joined, name);
  return joined;
}

/* A width copied for one instance: a `valof(len)` inside block `hdr` becomes `valof(hdr.len)`, so it
 * reads that instance's length and not some other field that happens to share the name. The tree is
 * copied rather than edited because the block's own copy is shared by every use. */
static Expr *rename_expr(Composer *composer, const Expr *expr, const char *prefix) {
  if (expr == NULL || prefix[0] == '\0') {
    return (Expr *)expr;
  }
  Expr *copy = region_alloc(composer->file->region, sizeof(Expr));
  if (copy == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return (Expr *)expr;
  }
  *copy = *expr;
  if (expr->kind == EXPR_VALOF) {
    copy->reference = join(composer, prefix, expr->reference);
  } else if (expr->kind == EXPR_BINARY) {
    copy->lhs = rename_expr(composer, expr->lhs, prefix);
    copy->rhs = rename_expr(composer, expr->rhs, prefix);
  }
  return copy;
}

static bool append(Composer *composer, FieldList *list, const AstField *field) {
  if (list->count >= COMPOSE_FIELD_MAX) {
    fail(composer, field->line, field->column, 0, "blocks here expand to more fields than any packet could hold");
    return false;
  }
  if (list->count == list->capacity) {
    size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
    AstField *grown = realloc(list->items, capacity * sizeof(*grown));
    if (grown == NULL) {
      fail(composer, 1, 1, 0, "out of memory");
      return false;
    }
    list->items = grown;
    list->capacity = capacity;
  }
  list->items[list->count++] = *field;
  return true;
}

/* Whether `block` has been declared by the time (line, column) is reached: from its closing brace on.
 * That is also the whole of the recursion check -- inside its own braces a block is not declared yet,
 * and neither is anything after it. */
static bool declared_by(const AstPacket *block, int line, int column) {
  return block->end_line < line || (block->end_line == line && block->end_column < column);
}

static void expand(Composer *composer, const AstPacket *container, const char *prefix, FieldList *out);

/* Check 25, for one use: each override must name a field of this instance, and only once. */
static void apply_overrides(
  Composer *composer,
  const AstField *use,
  const AstPacket *block,
  const char *prefix,
  FieldList *out,
  size_t start
) {
  for (const AstOverride *override = use->overrides; override != NULL; override = override->next) {
    char message[512];

    for (const AstOverride *earlier = use->overrides; earlier != override; earlier = earlier->next) {
      if (strcmp(earlier->name, override->name) == 0) {
        snprintf(message, sizeof(message), "'%s' fixes '%s' twice", use->name, override->name);
        fail(composer, override->line, override->column, 25, message);
        break;
      }
    }

    const char *full = join(composer, prefix, override->name);
    AstField *target = NULL;
    for (size_t i = start; i < out->count; ++i) {
      if (strcmp(out->items[i].name, full) == 0) {
        target = &out->items[i];
        break;
      }
    }
    if (target == NULL) {
      snprintf(
        message, sizeof(message), "block '%s' has no field '%s' for '%s' to fix", block->name, override->name, use->name
      );
      fail(composer, override->line, override->column, 25, message);
      continue;
    }

    /* A fixed value the block wrote is replaced, not contradicted: overriding it is the point. Check 8
     * still sees whether the new value fits, and check 9 whether the field can be fixed at all. */
    target->has_fixed = true;
    target->fixed = override->value;
    target->fixed_line = override->line;
    target->fixed_column = override->column;
  }
}

static void expand(Composer *composer, const AstPacket *container, const char *prefix, FieldList *out) {
  for (size_t f = 0; f < container->field_count; ++f) {
    const AstField *field = &container->fields[f];

    if (field->block == NULL) {
      AstField copy = *field;
      copy.name = join(composer, prefix, field->name);
      copy.width = rename_expr(composer, field->width, prefix);
      copy.reserve = rename_expr(composer, field->reserve, prefix);
      append(composer, out, &copy);
      continue;
    }

    char message[512];
    const AstPacket *block = find_block(composer->file, field->block);

    /* Check 2, for the one case the checker cannot see after expansion: an instance sharing its name
     * with another field, whose expanded names (`hdr.id` beside `hdr`) no longer collide. */
    for (size_t other = 0; other < container->field_count; ++other) {
      if (other != f && strcmp(container->fields[other].name, field->name) == 0
          && (other < f || container->fields[other].block == NULL)) {
        snprintf(message, sizeof(message), "'%s' already has a field called '%s'", container->name, field->name);
        fail(composer, field->line, field->column, 2, message);
        break;
      }
    }

    /* Check 23. */
    if (block == NULL) {
      snprintf(message, sizeof(message), "'%s' uses '%s', which is not a block", field->name, field->block);
      fail(composer, field->line, field->column, 23, message);
      continue;
    }

    /* Check 24. */
    if (!declared_by(block, field->line, field->column)) {
      if (block == container) {
        snprintf(message, sizeof(message), "block '%s' uses itself, through '%s'", block->name, field->name);
      } else {
        snprintf(
          message,
          sizeof(message),
          "'%s' uses block '%s' before it is declared; declare a block above everything that uses it",
          field->name,
          block->name
        );
      }
      fail(composer, field->line, field->column, 24, message);
      continue;
    }

    const char *instance = join(composer, prefix, field->name);
    const char *inner = join(composer, instance, ".");
    size_t start = out->count;
    expand(composer, block, inner, out);
    apply_overrides(composer, field, block, inner, out, start);
  }
}

bool compose_file(AstFile *file, DiagList *diags) {
  Composer composer = { .file = file, .diags = diags, .failed = false };

  for (size_t b = 0; b < file->block_count; ++b) {
    const AstPacket *block = &file->blocks[b];

    if (block->field_count == 0) {
      fail(&composer, block->line, block->column, 0, "a block must declare at least one field");
    }
    /* Names share nothing with packets -- a use can only name a block -- but two blocks of one name
     * would leave every use ambiguous. It is check 1's rule, applied to blocks. */
    for (size_t earlier = 0; earlier < b; ++earlier) {
      if (strcmp(file->blocks[earlier].name, block->name) == 0) {
        char message[256];
        snprintf(message, sizeof(message), "there is already a block called '%s'", block->name);
        fail(&composer, block->line, block->column, 1, message);
        break;
      }
    }
  }

  /* Each block on its own first, into nothing. A mistake inside a block -- a use of itself, an unknown
   * block, a bad override -- is then reported once, at the block, and not once per packet that uses
   * it; and a block no packet uses yet is held to the same rules. */
  for (size_t b = 0; b < file->block_count; ++b) {
    FieldList scratch = { 0 };
    expand(&composer, &file->blocks[b], "", &scratch);
    free(scratch.items);
  }
  if (composer.failed) {
    diag_sort(diags);
    return false;
  }

  /* Every packet is expanded even after one fails, so a file reports all of its composition mistakes
   * in one run -- the same promise the parser and the checks make. */
  for (size_t p = 0; p < file->packet_count; ++p) {
    AstPacket *packet = &file->packets[p];

    bool uses_a_block = false;
    for (size_t f = 0; f < packet->field_count; ++f) {
      uses_a_block = uses_a_block || packet->fields[f].block != NULL;
    }
    if (!uses_a_block) {
      continue;
    }

    FieldList out = { 0 };
    size_t reported = diags->count;
    expand(&composer, packet, "", &out);
    if (diags->count != reported) {
      free(out.items);
      continue;
    }

    free(packet->fields);
    packet->fields = out.items;
    packet->field_count = out.count;
    packet->field_capacity = out.capacity;
  }

  diag_sort(diags);
  return !composer.failed;
}
