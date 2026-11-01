#include "compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* More fields than any frame has, used to stop a file that nests blocks two-by-two a dozen deep from
 * expanding into millions. */
#define COMPOSE_FIELD_MAX 65536

/* The conditions one expand() has placed (#12): each written `@if`, and the copy its fields share. */
typedef struct {
  const struct AstCondition **from;
  struct AstCondition **to;
  size_t count;
  size_t capacity;
} Placed;

typedef struct {
  AstFile *file;
  DiagList *diags;
  bool failed;
  /* The expand() under way's, or NULL. */
  Placed *placed;
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

/* A term's insides, copied for an instance: the field each comparison reads renamed, as in place(). The
 * operands of an `||` belong to the one term, so they are copied with it rather than shared. */
static void copy_operands(Composer *composer, AstCondition *copy, const char *prefix) {
  if (copy->kind == CONDITION_COMPARE) {
    copy->field = rename_expr(composer, copy->field, prefix);
    return;
  }
  AstCondition *operands[2] = { copy->lhs, copy->rhs };
  for (int i = 0; i < 2; ++i) {
    AstCondition *operand = region_alloc(composer->file->region, sizeof(AstCondition));
    if (operand == NULL) {
      fail(composer, 1, 1, 0, "out of memory");
      return;
    }
    *operand = *operands[i];
    copy_operands(composer, operand, prefix);
    operands[i] = operand;
  }
  copy->lhs = operands[0];
  copy->rhs = operands[1];
}

/* A field's conditions (#12) as they apply where it lands: each `valof()` renamed for the instance, as
 * a width is, and the chain hung under `outer` -- the conditions of the use it arrived through -- so a
 * field of a block used under `@if` is present only when the use is. Copied even where nothing changes,
 * because what a name in a condition means depends on the packet it ends up in, and each copy is
 * resolved there (resolve_conditions()); one already resolved -- in its block -- stays resolved.
 *
 * Within one expand() every field under one `@if` shares one copy, so a group's fields carry the same
 * node: the checks then see a group as the run of fields carrying it, rather than comparing every field
 * with every other to find out. */
static AstCondition *place(Composer *composer, const AstCondition *condition, const char *prefix, AstCondition *outer) {
  if (condition == NULL) {
    return outer;
  }
  Placed *placed = composer->placed;
  for (size_t i = 0; placed != NULL && i < placed->count; ++i) {
    if (placed->from[i] == condition) {
      return placed->to[i];
    }
  }
  AstCondition *copy = region_alloc(composer->file->region, sizeof(AstCondition));
  if (copy == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return outer;
  }
  *copy = *condition;
  copy_operands(composer, copy, prefix);
  copy->parent = place(composer, condition->parent, prefix, outer);
  copy->text = NULL;

  if (placed != NULL) {
    if (placed->count == placed->capacity) {
      size_t capacity = placed->capacity == 0 ? 8 : placed->capacity * 2;
      const AstCondition **from = realloc(placed->from, capacity * sizeof(*from));
      if (from != NULL) {
        placed->from = from;
      }
      AstCondition **to = realloc(placed->to, capacity * sizeof(*to));
      if (to != NULL) {
        placed->to = to;
      }
      if (from == NULL || to == NULL) {
        fail(composer, 1, 1, 0, "out of memory");
        return copy;
      }
      placed->capacity = capacity;
    }
    placed->from[placed->count] = condition;
    placed->to[placed->count] = copy;
    placed->count += 1;
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

static void expand(
  Composer *composer,
  const AstPacket *container,
  const char *prefix,
  AstCondition *outer,
  FieldList *out
);

/* A "did you mean" for a use naming no block or enum: the closest name there is, imported ones included
 * under the name this file gives them -- so `link.Heder` can become `link.Header`. */
static void suggest_block(Composer *composer, size_t index, const AstField *use) {
  const AstFile *file = composer->file;
  const char **names = malloc((file->block_count + file->enum_count + 1) * sizeof(*names));
  if (names == NULL || use->block_line == 0) {
    free(names);
    return;
  }
  for (size_t i = 0; i < file->block_count; ++i) {
    names[i] = file->blocks[i].name;
  }
  for (size_t i = 0; i < file->enum_count; ++i) {
    names[file->block_count + i] = file->enums[i].name;
  }
  const char *closest = diag_closest(use->block, strlen(use->block), names, file->block_count + file->enum_count);
  if (closest != NULL) {
    diag_set_fix(
      composer->diags,
      index,
      use->block_line,
      use->block_column,
      use->block_line,
      use->block_column + (int)strlen(use->block),
      closest,
      "did you mean '%s'?",
      closest
    );
  }
  free(names);
}

/* A width's value when it is a constant: what an element's size is made of. False for anything with a
 * valof() in it, and for arithmetic that goes wrong -- the checks report those at the block itself. */
static bool constant_width(const Expr *expr, uint64_t *out) {
  if (expr == NULL) {
    return false;
  }
  if (expr->kind == EXPR_LITERAL) {
    *out = expr->literal;
    return true;
  }
  uint64_t lhs = 0;
  uint64_t rhs = 0;
  if (expr->kind != EXPR_BINARY || !constant_width(expr->lhs, &lhs) || !constant_width(expr->rhs, &rhs)) {
    return false;
  }
  switch (expr->op) {
  case '+':
    *out = lhs + rhs;
    return *out >= lhs;
  case '-':
    *out = lhs - rhs;
    return rhs <= lhs;
  case '*':
    *out = lhs * rhs;
    return lhs == 0 || *out / lhs == rhs;
  default:
    *out = rhs == 0 ? 0 : lhs / rhs;
    return rhs != 0;
  }
}

/* `name: Block[count];` (#55) becomes one field: a bit string `count` elements long, each element laid
 * out as the block's fields are. Everything downstream -- the checks, the layout, the bytecode, the
 * walk -- sees an ordinary field whose width is `count * element_bits`, with the reservation a
 * valof() width needs when the count is a field's. Only the accessors look inside, at `element`.
 *
 * Every element has to be the same size, and a whole number of bytes: the first so that element i
 * is at i times that size, the second so that however many elements a frame carries, the packet still
 * ends on a byte boundary (check 32). */
static void repeat(
  Composer *composer,
  const AstField *use,
  const AstPacket *block,
  const char *prefix,
  AstCondition *outer,
  FieldList *out
) {
  char message[512];

  /* Check 33: an override fixes one field of one instance, and a repetition is many. */
  if (use->overrides != NULL) {
    snprintf(
      message,
      sizeof(message),
      "'%s' repeats block '%s', so there is no one instance to fix a field of; fix it in the block",
      use->name,
      block->name
    );
    fail(composer, use->overrides->line, use->overrides->column, 33, message);
    return;
  }

  uint64_t element_bits = 0;
  for (size_t i = 0; i < block->field_count; ++i) {
    /* Check 32, for a field of the block present only in some elements (#12): element i would no longer
     * be i times one size in. The repetition as a whole may be conditional; a part of an element not. */
    if (block->fields[i].condition != NULL) {
      snprintf(
        message,
        sizeof(message),
        "'%s' repeats block '%s', whose field '%s' is conditional; every element must be the same size, "
        "so a repeated block cannot leave a field out",
        use->name,
        block->name,
        block->fields[i].name
      );
      fail(composer, use->repeat_line, use->repeat_column, 32, message);
      return;
    }
    uint64_t width = 0;
    if (!constant_width(block->fields[i].width, &width)) {
      snprintf(
        message,
        sizeof(message),
        "'%s' repeats block '%s', whose field '%s' has no constant width; every element must be the same "
        "size",
        use->name,
        block->name,
        block->fields[i].name
      );
      fail(composer, use->repeat_line, use->repeat_column, 32, message);
      return;
    }
    element_bits += width;
  }
  if (element_bits == 0 || element_bits % 8 != 0) {
    snprintf(
      message,
      sizeof(message),
      "'%s' repeats block '%s', which is %llu bits; a repeated block must be a whole number of bytes, so "
      "that any number of them is too",
      use->name,
      block->name,
      (unsigned long long)element_bits
    );
    fail(composer, use->repeat_line, use->repeat_column, 32, message);
    return;
  }

  Expr *count = rename_expr(composer, use->repeat, prefix);
  Expr *size = region_alloc(composer->file->region, sizeof(Expr));
  Expr *width = region_alloc(composer->file->region, sizeof(Expr));
  if (size == NULL || width == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return;
  }
  *size =
    (Expr){ .kind = EXPR_LITERAL, .line = use->repeat_line, .column = use->repeat_column, .literal = element_bits };
  *width = (Expr){
    .kind = EXPR_BINARY, .line = use->repeat_line, .column = use->repeat_column, .op = '*', .lhs = count, .rhs = size
  };

  AstField field = *use;
  field.name = join(composer, prefix, use->name);
  field.block = NULL;
  field.repeat = NULL;
  field.overrides = NULL;
  field.element = block;
  field.element_bits = element_bits;
  field.width = width;
  field.reserve = rename_expr(composer, use->reserve, prefix);
  field.condition = place(composer, use->condition, prefix, outer);
  append(composer, out, &field);
}

/* ---- Enumerations (#11) ------------------------------------------------------------------------
 *
 * A field typed by an enum, `level: TxLevel;`, is written like a use of a block and is resolved here,
 * where it is known which of the two a name is. It becomes an ordinary field of the enum's width that
 * remembers its type, with a value written as a name looked up in that type -- so nothing after this
 * lays it out, checks it or walks it any differently from `level: 3;`. */

/* A "did you mean" among `enumeration`'s names, for one written at (line, column) that it lacks. */
static void suggest_item(
  Composer *composer,
  size_t index,
  const AstEnum *enumeration,
  const char *wrong,
  int line,
  int column
) {
  const char **names = malloc((enumeration->item_count + 1) * sizeof(*names));
  if (names == NULL || line == 0) {
    free(names);
    return;
  }
  for (size_t i = 0; i < enumeration->item_count; ++i) {
    names[i] = enumeration->items[i].name;
  }
  const char *closest = diag_closest(wrong, strlen(wrong), names, enumeration->item_count);
  if (closest != NULL) {
    diag_set_fix(
      composer->diags, index, line, column, line, column + (int)strlen(wrong), closest, "did you mean '%s'?", closest
    );
  }
  free(names);
}

/* Check 39: the value a field typed by `enumeration` is given, as a name or as a number. `what` names
 * the field in the message. Sets `*value` and returns true when it is a name the enum lists. */
static bool enum_value(
  Composer *composer,
  const AstEnum *enumeration,
  const char *what,
  const char *name,
  uint64_t number,
  int line,
  int column,
  uint64_t *value
) {
  char message[512];
  if (name == NULL) {
    /* A number the enum lists is one somebody knows the name of; say it, so the fix is to copy it. */
    const char *listed = ast_enum_name_of(enumeration, number);
    if (listed != NULL) {
      snprintf(
        message,
        sizeof(message),
        "'%s' is typed by enum '%s', so its value is one of the enum's names: write '%s' for %llu",
        what,
        enumeration->name,
        listed,
        (unsigned long long)number
      );
    } else {
      snprintf(
        message,
        sizeof(message),
        "'%s' is typed by enum '%s', so its value is one of the enum's names, not a number",
        what,
        enumeration->name
      );
    }
    fail(composer, line, column, 39, message);
    return false;
  }

  const AstEnumItem *item = ast_enum_item(enumeration, name);
  if (item == NULL) {
    size_t index = composer->diags->count;
    snprintf(message, sizeof(message), "enum '%s' has no name '%s' for '%s' to take", enumeration->name, name, what);
    fail(composer, line, column, 39, message);
    suggest_item(composer, index, enumeration, name, line, column);
    return false;
  }
  *value = item->value;
  return true;
}

/* Check 39 from the other side: a name is a value only of a field typed by an enum. */
static void refuse_name(Composer *composer, const char *what, const char *name, int line, int column) {
  char message[512];
  snprintf(
    message,
    sizeof(message),
    "'%s' is not typed by an enum, so its value is a number and '%s' names nothing; to give its values "
    "names, declare an enum and write 'name: Enum;'",
    what,
    name
  );
  fail(composer, line, column, 39, message);
}

/* `name: Enum [= NAME];` becomes an ordinary field of the enum's width. */
static void type_by_enum(
  Composer *composer,
  const AstField *use,
  const AstEnum *enumeration,
  const char *prefix,
  AstCondition *outer,
  FieldList *out
) {
  char message[512];

  /* Check 41: an enum is a field's type, and there is nothing inside it to paste, fix or repeat. */
  if (use->anonymous) {
    snprintf(
      message,
      sizeof(message),
      "'%s' is an enum, which is the type of a field and has no fields to paste in; name the field, "
      "'name: %s;'",
      use->block,
      use->block
    );
    fail(composer, use->line, use->column, 41, message);
    return;
  }
  if (use->repeat != NULL) {
    snprintf(
      message, sizeof(message), "'%s' repeats enum '%s'; only a block can be repeated", use->name, enumeration->name
    );
    fail(composer, use->repeat_line, use->repeat_column, 41, message);
    return;
  }
  if (use->overrides != NULL) {
    snprintf(
      message,
      sizeof(message),
      "'%s' is typed by enum '%s', which has no fields to fix; give its value as '%s: %s = NAME;'",
      use->name,
      enumeration->name,
      use->name,
      use->block
    );
    fail(composer, use->overrides->line, use->overrides->column, 41, message);
    return;
  }

  Expr *width = region_alloc(composer->file->region, sizeof(Expr));
  if (width == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return;
  }
  *width =
    (Expr){ .kind = EXPR_LITERAL, .line = use->block_line, .column = use->block_column, .literal = enumeration->width };

  AstField field = *use;
  field.name = join(composer, prefix, use->name);
  field.block = NULL;
  field.enumeration = enumeration->name;
  field.width = width;
  field.reserve = rename_expr(composer, use->reserve, prefix);
  field.condition = place(composer, use->condition, prefix, outer);
  if (use->has_fixed || use->fixed_name != NULL) {
    uint64_t value = 0;
    field.has_fixed = enum_value(
      composer, enumeration, use->name, use->fixed_name, use->fixed, use->fixed_line, use->fixed_column, &value
    );
    field.fixed = value;
    field.fixed_name = NULL;
  }
  if (use->has_default || use->default_name != NULL) {
    uint64_t value = 0;
    field.has_default = enum_value(
      composer,
      enumeration,
      use->name,
      use->default_name,
      use->default_value,
      use->default_line,
      use->default_column,
      &value
    );
    field.default_value = value;
    field.default_name = NULL;
  }
  append(composer, out, &field);
}

/* What a use of a block cannot carry, and a field typed by an enum can -- so the parser takes them
 * from either and this refuses them once the name is known to be a block's. A group in front of a use
 * would have to reach inside the block and change its layout, which is settled where the block is
 * declared so that the same block reads the same everywhere. */
static bool block_use_is_plain(Composer *composer, const AstField *use) {
  char message[512];
  bool plain = true;
  if (use->bit_order_written || use->byte_order_written || use->cal != NULL) {
    int line = use->bit_order_written    ? use->bit_order_line
               : use->byte_order_written ? use->byte_order_line
                                         : use->cal_line;
    int column = use->bit_order_written    ? use->bit_order_column
                 : use->byte_order_written ? use->byte_order_column
                                           : use->cal_column;
    snprintf(
      message,
      sizeof(message),
      "'%s' uses a block, whose layout is fixed where the block is declared; put the attribute there",
      use->name
    );
    fail(composer, line, column, 0, message);
    plain = false;
  }
  /* A repetition counted by a field reserves its room like any valof()-sized field; a single instance
   * has none of its own, since its fields reserve what they need. */
  if (use->reserve != NULL && use->repeat == NULL) {
    snprintf(
      message,
      sizeof(message),
      "'%s' uses a block, which has no reservation of its own; its fields reserve what they need",
      use->name
    );
    fail(composer, use->reserve_line, use->reserve_column, 0, message);
    plain = false;
  }
  if (use->has_fixed || use->fixed_name != NULL) {
    snprintf(
      message,
      sizeof(message),
      "'%s' uses a block, and a block has no single value to fix; fix its fields in '{ ... }' instead",
      use->name
    );
    fail(composer, use->fixed_line, use->fixed_column, 0, message);
    plain = false;
  }
  if (use->has_default || use->default_name != NULL) {
    snprintf(
      message,
      sizeof(message),
      "'%s' uses a block, and a block has no single value to default; give its fields one in '{ ... }' instead",
      use->name
    );
    fail(composer, use->default_line, use->default_column, 0, message);
    plain = false;
  }
  return plain;
}

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

    /* A field typed by an enum takes one of its names, and only such a field takes a name (#11). */
    uint64_t value = override->value;
    const AstEnum *enumeration = ast_find_enum(composer->file, target->enumeration);
    if (enumeration != NULL) {
      if (!enum_value(
            composer,
            enumeration,
            full,
            override->value_name,
            override->value,
            override->value_line,
            override->value_column,
            &value
          )) {
        continue;
      }
    } else if (override->value_name != NULL) {
      refuse_name(composer, full, override->value_name, override->value_line, override->value_column);
      continue;
    }

    /* What the block wrote is replaced, not contradicted: overriding it is the point, and what the use
     * wrote is what the field becomes (#144). Check 8 still sees whether the new value fits, and check 9
     * whether the field can be fixed at all. */
    target->has_fixed = !override->is_default;
    target->has_default = override->is_default;
    if (override->is_default) {
      target->default_value = value;
      target->default_line = override->line;
      target->default_column = override->column;
    } else {
      target->fixed = value;
      target->fixed_line = override->line;
      target->fixed_column = override->column;
    }
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

static void expand(
  Composer *composer,
  const AstPacket *container,
  const char *prefix,
  AstCondition *outer,
  FieldList *out
) {
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
  Placed placed = { 0 };
  Placed *enclosing = composer->placed;
  composer->placed = &placed;

  for (size_t f = 0; f < container->field_count; ++f) {
    const AstField *field = &container->fields[f];
    starts[f] = out->count;

    if (field->block == NULL) {
      if (field->fixed_name != NULL) {
        refuse_name(composer, field->name, field->fixed_name, field->fixed_line, field->fixed_column);
        continue;
      }
      if (field->default_name != NULL) {
        refuse_name(composer, field->name, field->default_name, field->default_line, field->default_column);
        continue;
      }
      AstField copy = *field;
      copy.name = join(composer, prefix, field->name);
      copy.width = rename_expr(composer, field->width, prefix);
      copy.reserve = rename_expr(composer, field->reserve, prefix);
      copy.condition = place(composer, field->condition, prefix, outer);
      append(composer, out, &copy);
      continue;
    }

    char message[512];
    const AstPacket *block = find_block(composer->file, field->block);
    const AstEnum *enumeration = block == NULL ? ast_find_enum(composer->file, field->block) : NULL;

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

    if (enumeration != NULL) {
      /* Check 24, for an enum: declared above what uses it, like a block, since it is used like one. */
      if (enumeration->end_line > field->line
          || (enumeration->end_line == field->line && enumeration->end_column >= field->column)) {
        snprintf(
          message,
          sizeof(message),
          "'%s' uses enum '%s' before it is declared; declare an enum above everything that uses it",
          field->name,
          enumeration->name
        );
        fail(composer, field->line, field->column, 24, message);
        continue;
      }
      type_by_enum(composer, field, enumeration, prefix, outer, out);
      continue;
    }

    /* Check 23. Written without a width, a field and an anonymous use look alike, so the message for
     * one says what the other would have needed. */
    if (block == NULL) {
      size_t index = composer->diags->count;
      if (field->anonymous) {
        snprintf(
          message, sizeof(message), "'%s' is not a block; a field needs a width, '%s: 8;'", field->block, field->block
        );
      } else {
        snprintf(
          message, sizeof(message), "'%s' uses '%s', which is not a block or an enum", field->name, field->block
        );
      }
      fail(composer, field->line, field->column, 23, message);
      suggest_block(composer, index, field);
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

    if (!block_use_is_plain(composer, field)) {
      continue;
    }

    if (field->repeat != NULL) {
      repeat(composer, field, block, prefix, outer, out);
      continue;
    }

    /* Anonymous, the block's fields land under whatever prefix the use itself sits under. */
    const char *inner = field->anonymous ? prefix : join(composer, join(composer, prefix, field->name), ".");
    size_t start = out->count;
    AstCondition *use_condition = place(composer, field->condition, prefix, outer);
    expand(composer, block, inner, use_condition, out);
    apply_overrides(composer, field, block, inner, out, start);
    if (field->anonymous) {
      check_pasted(composer, container, f, out, starts);
    }
  }
  free(starts);
  free(placed.from);
  free(placed.to);
  composer->placed = enclosing;
}

/* Checks 1 and 35 to 38, over the enums this file declares; an imported one was checked in its own. */
static void check_enums(Composer *composer) {
  const AstFile *file = composer->file;
  char message[512];

  for (size_t e = 0; e < file->enum_count; ++e) {
    const AstEnum *enumeration = &file->enums[e];

    /* Check 1 for a block of this file named like an imported enum: reported at the block, the one
     * written here. Everything else about an imported enum was checked in its own file. */
    if (enumeration->origin != NULL) {
      for (size_t b = 0; b < file->block_count; ++b) {
        const AstPacket *block = &file->blocks[b];
        if (block->origin == NULL && strcmp(block->name, enumeration->name) == 0) {
          snprintf(
            message,
            sizeof(message),
            "there is already an enum called '%s', from '%s'; a field's type is a block or an enum, so the "
            "two share their names",
            enumeration->name,
            enumeration->origin
          );
          fail(composer, block->line, block->column, 1, message);
        }
      }
      continue;
    }

    /* Check 1: a field's type is a block or an enum, and one name cannot be both. */
    for (size_t earlier = 0; earlier < e; ++earlier) {
      if (strcmp(file->enums[earlier].name, enumeration->name) == 0) {
        snprintf(message, sizeof(message), "there is already an enum called '%s'", enumeration->name);
        fail(composer, enumeration->line, enumeration->column, 1, message);
        break;
      }
    }
    for (size_t b = 0; b < file->block_count; ++b) {
      const AstPacket *block = &file->blocks[b];
      if (strcmp(block->name, enumeration->name) != 0) {
        continue;
      }
      /* Reported at the later of the two, which is the one written knowing the other was there; an
       * imported block counts as written first, above everything in this file. */
      const bool block_later = block->origin == NULL && block->line > enumeration->line;
      snprintf(
        message,
        sizeof(message),
        "there is already %s called '%s'; a field's type is a block or an enum, so the two share their names",
        block_later ? "an enum" : "a block",
        enumeration->name
      );
      fail(
        composer,
        block_later ? block->line : enumeration->line,
        block_later ? block->column : enumeration->column,
        1,
        message
      );
      break;
    }

    /* Check 35. */
    const bool width_ok = enumeration->width >= 1 && enumeration->width <= 64;
    if (!width_ok) {
      snprintf(
        message,
        sizeof(message),
        "enum '%s' is %llu bits wide; an enum's width is 1 to 64 bits, since a field typed by it is a number",
        enumeration->name,
        (unsigned long long)enumeration->width
      );
      fail(composer, enumeration->width_line, enumeration->width_column, 35, message);
    }

    for (size_t i = 0; i < enumeration->item_count; ++i) {
      const AstEnumItem *item = &enumeration->items[i];

      /* Check 36. */
      for (size_t j = 0; j < i; ++j) {
        if (strcmp(enumeration->items[j].name, item->name) == 0) {
          snprintf(
            message,
            sizeof(message),
            "enum '%s' already has a name '%s', on line %d",
            enumeration->name,
            item->name,
            enumeration->items[j].line
          );
          fail(composer, item->line, item->column, 36, message);
          break;
        }
      }

      /* Check 38. A counted value past the largest there is has no value at all, so it is not compared
       * with anything else. */
      if (item->overflowed) {
        snprintf(
          message,
          sizeof(message),
          "'%s' in enum '%s' counts past the largest 64-bit value; give it a value of its own",
          item->name,
          enumeration->name
        );
        fail(composer, item->line, item->column, 38, message);
        continue;
      }
      if (width_ok && enumeration->width < 64 && item->value >> enumeration->width != 0) {
        snprintf(
          message,
          sizeof(message),
          "'%s' is %llu, which enum '%s' cannot hold in %llu bits",
          item->name,
          (unsigned long long)item->value,
          enumeration->name,
          (unsigned long long)enumeration->width
        );
        fail(composer, item->line, item->column, 38, message);
      }

      /* Check 37: against the name before it, which is enough when every step is checked. */
      const AstEnumItem *before = i > 0 ? &enumeration->items[i - 1] : NULL;
      if (before != NULL && !before->overflowed && item->value <= before->value) {
        snprintf(
          message,
          sizeof(message),
          "'%s' is %llu, which does not come after '%s' (%llu); an enum's values increase, so it reads in "
          "order and no value is listed twice",
          item->name,
          (unsigned long long)item->value,
          before->name,
          (unsigned long long)before->value
        );
        fail(composer, item->line, item->column, 37, message);
      }
    }
  }
}

/* ---- Conditions (#12, #126) ------------------------------------------------------------------------
 *
 * A comparison compares a field's value with a number, or -- on a field typed by an enum -- with one of
 * its names, as a fixed value does (check 39). Which it is depends on the field it names, known only
 * once the packet or block is flat, so it is looked up here. A name naming no earlier field is check
 * 7's, reported by the checker; it is left alone here. */

static bool is_ordering(CompareOp op) {
  return op != COMPARE_EQ && op != COMPARE_NE;
}

static const char *compare_spelling(CompareOp op) {
  switch (op) {
  case COMPARE_EQ:
    return "==";
  case COMPARE_NE:
    return "!=";
  case COMPARE_LT:
    return "<";
  case COMPARE_LE:
    return "<=";
  case COMPARE_GT:
    return ">";
  case COMPARE_GE:
    return ">=";
  }
  return "==";
}

static void resolve_comparison(Composer *composer, const AstField *fields, size_t before, AstCondition *condition) {
  if (condition->kind != CONDITION_COMPARE) {
    resolve_comparison(composer, fields, before, condition->lhs);
    resolve_comparison(composer, fields, before, condition->rhs);
    return;
  }
  if (condition->resolved || condition->field == NULL) {
    return;
  }
  const AstField *compared = NULL;
  for (size_t j = 0; j < before && compared == NULL; ++j) {
    if (strcmp(fields[j].name, condition->field->reference) == 0) {
      compared = &fields[j];
    }
  }
  if (compared == NULL) {
    return;
  }
  condition->resolved = true;

  const AstEnum *enumeration = ast_find_enum(composer->file, compared->enumeration);
  if (enumeration == NULL) {
    if (condition->value_name != NULL) {
      refuse_name(composer, compared->name, condition->value_name, condition->value_line, condition->value_column);
    }
    return;
  }
  /* Check 43 (#126): an enum's values are names, not quantities, so they are equal or not -- whatever
   * order or bits the numbers behind them happen to have. */
  if (condition->has_mask || is_ordering(condition->op)) {
    const char *written = condition->field->written != NULL ? condition->field->written : compared->name;
    const char *first = enumeration->item_count > 0 ? enumeration->items[0].name : "A";
    const char *second = enumeration->item_count > 1 ? enumeration->items[1].name : first;
    char message[512];
    snprintf(
      message,
      sizeof(message),
      "'%s' is typed by enum '%s', whose values are names: a condition compares it only with '==' or "
      "'!=', so write each name that qualifies, 'valof(%s) == %s || valof(%s) == %s'",
      compared->name,
      enumeration->name,
      written,
      first,
      written,
      second
    );
    fail(
      composer,
      condition->has_mask ? condition->mask_line : condition->line,
      condition->has_mask ? condition->mask_column : condition->column,
      43,
      message
    );
    return;
  }
  uint64_t value = 0;
  if (enum_value(
        composer,
        enumeration,
        compared->name,
        condition->value_name,
        condition->value,
        condition->value_line,
        condition->value_column,
        &value
      )) {
    condition->value = value;
  }
}

static void resolve_conditions(Composer *composer, const AstField *fields, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    for (AstCondition *condition = fields[i].condition; condition != NULL; condition = condition->parent) {
      resolve_comparison(composer, fields, i, condition);
    }
  }
}

/* Past this, a description ends in " && ...": each nested condition's text holds the whole chain above
 * it, so without a limit the text would grow with the square of how deeply conditions nest. */
#define CONDITION_TEXT_MAX 256

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
  bool failed;
} Spelling;

static void spell(Spelling *out, const char *piece) {
  size_t length = strlen(piece);
  if (out->length + length + 1 > out->capacity) {
    size_t capacity = (out->capacity + length + 1) * 2;
    char *text = realloc(out->text, capacity);
    if (text == NULL) {
      out->failed = true;
      return;
    }
    out->text = text;
    out->capacity = capacity;
  }
  memcpy(out->text + out->length, piece, length + 1);
  out->length += length;
}

/* One term, as written but spaced as the formatter spaces it: an `&&` inside an `||` in parentheses,
 * which it needs no more than `1 + 2 * 3` does, but which is how anyone would read it. */
static void spell_condition(Spelling *out, const AstCondition *condition, bool inside_or) {
  if (condition->kind == CONDITION_COMPARE) {
    char number[64];
    spell(out, "valof(");
    spell(out, condition->field != NULL && condition->field->reference != NULL ? condition->field->reference : "");
    spell(out, ")");
    if (condition->has_mask) {
      snprintf(number, sizeof(number), " & %#llx", (unsigned long long)condition->mask);
      spell(out, number);
    }
    spell(out, " ");
    spell(out, compare_spelling(condition->op));
    spell(out, " ");
    if (condition->value_name != NULL) {
      spell(out, condition->value_name);
    } else {
      snprintf(number, sizeof(number), "%llu", (unsigned long long)condition->value);
      spell(out, number);
    }
    return;
  }
  bool parenthesised = condition->kind == CONDITION_AND && inside_or;
  if (parenthesised) {
    spell(out, "(");
  }
  spell_condition(out, condition->lhs, condition->kind == CONDITION_OR);
  spell(out, condition->kind == CONDITION_OR ? " || " : " && ");
  spell_condition(out, condition->rhs, condition->kind == CONDITION_OR);
  if (parenthesised) {
    spell(out, ")");
  }
}

/* What FramedslField.condition shows: the chain's terms joined by " && ", outermost first, an `||` in
 * parentheses once there is more than one term. */
static const char *condition_text(Composer *composer, AstCondition *condition) {
  if (condition->text != NULL) {
    return condition->text;
  }
  size_t count = 0;
  for (const AstCondition *term = condition; term != NULL; term = term->parent) {
    count += 1;
  }
  const AstCondition **terms = malloc(count * sizeof(*terms));
  if (terms == NULL) {
    fail(composer, 1, 1, 0, "out of memory");
    return "";
  }
  size_t k = count;
  for (const AstCondition *term = condition; term != NULL; term = term->parent) {
    terms[--k] = term;
  }

  Spelling out = { 0 };
  spell(&out, "");
  for (size_t t = 0; t < count; ++t) {
    if (t > 0 && out.length > CONDITION_TEXT_MAX) {
      spell(&out, " && ...");
      break;
    }
    bool parenthesised = count > 1 && terms[t]->kind == CONDITION_OR;
    spell(&out, t > 0 ? " && " : "");
    spell(&out, parenthesised ? "(" : "");
    spell_condition(&out, terms[t], false);
    spell(&out, parenthesised ? ")" : "");
  }
  free(terms);

  char *text = out.failed ? NULL : region_alloc(composer->file->region, out.length + 1);
  if (text == NULL) {
    free(out.text);
    fail(composer, 1, 1, 0, "out of memory");
    return "";
  }
  memcpy(text, out.text, out.length + 1);
  free(out.text);
  condition->text = text;
  return text;
}

static void describe_conditions(Composer *composer, const AstPacket *container) {
  for (size_t f = 0; f < container->field_count; ++f) {
    if (container->fields[f].condition != NULL) {
      condition_text(composer, container->fields[f].condition);
    }
  }
}

bool compose_file(AstFile *file, DiagList *diags) {
  Composer composer = { .file = file, .diags = diags, .failed = false };

  /* A mistake in an enum is its own: it does not make the blocks and packets using it any less worth
   * expanding, so it does not stop them being expanded below. */
  check_enums(&composer);
  const bool enums_failed = composer.failed;
  composer.failed = false;

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
    expand(&composer, &file->blocks[b], "", NULL, &expanded);
    resolve_conditions(&composer, expanded.items, expanded.count);
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
    describe_conditions(&composer, &file->blocks[b]);
  }
  if (composer.failed) {
    diag_sort(diags);
    return false;
  }

  /* Every packet is expanded even after one fails, so a file reports all of its composition mistakes
   * in one run -- the same promise the parser and the checks make. */
  for (size_t p = 0; p < file->packet_count; ++p) {
    AstPacket *packet = &file->packets[p];

    /* A value written as a name has to be looked up, or refused, even in a packet that uses nothing. */
    bool uses_a_block = false;
    for (size_t f = 0; f < packet->field_count; ++f) {
      uses_a_block = uses_a_block || packet->fields[f].block != NULL || packet->fields[f].fixed_name != NULL
                     || packet->fields[f].default_name != NULL || packet->fields[f].condition != NULL;
    }
    if (!uses_a_block) {
      continue;
    }

    FieldList out = { 0 };
    size_t reported = diags->count;
    expand(&composer, packet, "", NULL, &out);
    resolve_conditions(&composer, out.items, out.count);
    if (diags->count != reported) {
      free(out.items);
      continue;
    }

    free(packet->fields);
    packet->fields = out.items;
    packet->field_count = out.count;
    packet->field_capacity = out.capacity;
    describe_conditions(&composer, packet);
  }

  diag_sort(diags);
  return !composer.failed && !enums_failed;
}
