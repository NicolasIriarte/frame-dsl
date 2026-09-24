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

/* Check 29 (src/check.c), applied here too and not only there: the duplicate-name scan just below is
 * quadratic in `container->field_count`, and that is the count as declared, before COMPOSE_FIELD_MAX
 * or the checker ever sees it -- a container that declares this many fields directly pays the O(n^2)
 * cost right here regardless of what either would have said about the result. Found by fuzzing (#59).
 * Kept equal to check.c's ceiling: a container over it always expands to a packet over it too, so this
 * never rejects anything the checker would not have. */
#define COMPOSE_DECLARED_FIELD_CEILING 4096

/* Check 2, for an anonymous use: each field it pasted in, `out[starts[use]..]`, against every other
 * field of the container it was pasted into -- those before it, already in `out`, and the ordinary
 * fields after it, not expanded yet. A clash with something pasted in later is found when that is
 * pasted. Reported at the use, since that is the line to change; the block is fine on its own.
 *
 * `starts[k]` is where the container's k-th declaration began in `out`, for every k up to `use`, so
 * that a clash with an earlier use names the line of that use rather than of the block's declaration.
 *
 * Quadratic in the container's fields, so skipped once they pass check 29's ceiling: the checker
 * rejects a packet that large anyway, before its own quadratic duplicate scan. */
static void check_pasted(
  Composer *composer,
  const AstPacket *container,
  size_t use,
  const FieldList *out,
  const size_t *starts
) {
  const size_t first = starts[0];
  const size_t start = starts[use];
  if (out->count - first > COMPOSE_DECLARED_FIELD_CEILING) {
    return;
  }
  const AstField *pasting = &container->fields[use];

  for (size_t i = start; i < out->count; ++i) {
    const char *name = out->items[i].name;
    const AstField *clash = NULL;

    for (size_t k = 0; k < use && clash == NULL; ++k) {
      for (size_t j = starts[k]; j < starts[k + 1]; ++j) {
        if (strcmp(out->items[j].name, name) == 0) {
          clash = &container->fields[k];
          break;
        }
      }
    }
    for (size_t later = use + 1; later < container->field_count && clash == NULL; ++later) {
      const AstField *sibling = &container->fields[later];
      if (sibling->block == NULL && strcmp(sibling->name, name) == 0) {
        clash = sibling;
      }
    }
    if (clash == NULL) {
      continue;
    }

    char message[512];
    snprintf(
      message,
      sizeof(message),
      "'%s' pastes in '%s', but '%s' already has a field called '%s', from line %d",
      pasting->block,
      name,
      container->name,
      name,
      clash->line
    );
    fail(composer, pasting->line, pasting->column, 2, message);
  }
}

static void expand(Composer *composer, const AstPacket *container, const char *prefix, FieldList *out) {
  if (container->field_count > COMPOSE_DECLARED_FIELD_CEILING) {
    char message[256];
    snprintf(
      message, sizeof(message), "'%s' declares more fields than a packet or block may ever have", container->name
    );
    fail(composer, container->line, container->column, 29, message);
    return;
  }

  /* Where each declaration's fields begin in `out`, for check_pasted(). */
  size_t *starts = malloc((container->field_count + 1) * sizeof(*starts));
  if (starts == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return;
  }

  for (size_t f = 0; f < container->field_count; ++f) {
    const AstField *field = &container->fields[f];
    starts[f] = out->count;

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
     * with another field, whose expanded names (`hdr.id` beside `hdr`) no longer collide. An anonymous
     * use has no name of its own to collide; what it pastes in is compared in check_pasted(). */
    for (size_t other = 0; other < container->field_count && !field->anonymous; ++other) {
      const AstField *sibling = &container->fields[other];
      if (other != f && !sibling->anonymous && strcmp(sibling->name, field->name) == 0
          && (other < f || sibling->block == NULL)) {
        snprintf(message, sizeof(message), "'%s' already has a field called '%s'", container->name, field->name);
        fail(composer, field->line, field->column, 2, message);
        break;
      }
    }

    /* Check 23. Written without a width, a field and an anonymous use look alike, so the message for
     * one says what the other would have needed. */
    if (block == NULL) {
      if (field->anonymous) {
        snprintf(
          message, sizeof(message), "'%s' is not a block; a field needs a width, '%s: 8;'", field->block, field->block
        );
      } else {
        snprintf(message, sizeof(message), "'%s' uses '%s', which is not a block", field->name, field->block);
      }
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

    /* Anonymous, the block's fields land under whatever prefix the use itself sits under. */
    const char *inner = field->anonymous ? prefix : join(composer, join(composer, prefix, field->name), ".");
    size_t start = out->count;
    expand(composer, block, inner, out);
    apply_overrides(composer, field, block, inner, out, start);
    if (field->anonymous) {
      check_pasted(composer, container, f, out, starts);
    }
  }
  free(starts);
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
    FieldList expanded = { 0 };
    size_t reported = diags->count;
    expand(&composer, &file->blocks[b], "", &expanded);
    if (diags->count != reported) {
      free(expanded.items);
      continue;
    }

    /* The block keeps its expanded form. Expanding it again for each use gives the same result -- it
     * has no uses left in it -- and the checks can then judge it on its own, like a packet. Blocks
     * are expanded in the order they are declared, so a block using an earlier one gets it already
     * flat. */
    free(file->blocks[b].fields);
    file->blocks[b].fields = expanded.items;
    file->blocks[b].field_count = expanded.count;
    file->blocks[b].field_capacity = expanded.capacity;
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
