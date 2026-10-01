#include "calibration.h"

#include <math.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reading the tree -----------------------------------------------------------------------
 *
 * Check 20 has ruled out recursion, so every walk through a call below terminates. */

static const AstCal *callee_of(const AstFile *file, const CalExpr *call) {
  for (size_t i = 0; i < file->cal_count; ++i) {
    if (strcmp(file->cals[i].name, call->name) == 0) {
      return &file->cals[i];
    }
  }
  return NULL; /* unreachable after check 18 */
}

/* Whether a subtree's value depends on the argument at all. A call depends on it only if its argument
 * does *and* the curve it calls uses its own parameter. */
static bool depends(const AstFile *file, const CalExpr *expr) {
  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    return false;
  case CAL_EXPR_NAME:
    return true;
  case CAL_EXPR_NEGATE:
    return depends(file, expr->lhs);
  case CAL_EXPR_BINARY:
    return depends(file, expr->lhs) || depends(file, expr->rhs);
  case CAL_EXPR_CALL:
    return depends(file, expr->lhs) && depends(file, callee_of(file, expr)->body);
  }
  return true;
}

/* The tree evaluated directly, for folding a subtree that does not depend on the argument. */
static double tree_eval(const AstFile *file, const CalExpr *expr, double x) {
  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    return expr->number;
  case CAL_EXPR_NAME:
    return x;
  case CAL_EXPR_NEGATE:
    return -tree_eval(file, expr->lhs, x);
  case CAL_EXPR_CALL:
    return tree_eval(file, callee_of(file, expr)->body, tree_eval(file, expr->lhs, x));
  case CAL_EXPR_BINARY: {
    double lhs = tree_eval(file, expr->lhs, x);
    double rhs = tree_eval(file, expr->rhs, x);
    switch (expr->op) {
    case '+':
      return lhs + rhs;
    case '-':
      return lhs - rhs;
    case '*':
      return lhs * rhs;
    default:
      return lhs / rhs;
    }
  }
  }
  return 0;
}

/* Whether a subtree is `a * x + b`, and if so what a and b are. Most curves are: a gain and an offset,
 * written however the datasheet wrote them, through however many named steps. */
static bool affine(const AstFile *file, const CalExpr *expr, double *a, double *b) {
  double la;
  double lb;
  double ra;
  double rb;

  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    *a = 0;
    *b = expr->number;
    return true;
  case CAL_EXPR_NAME:
    *a = 1;
    *b = 0;
    return true;
  case CAL_EXPR_NEGATE:
    if (!affine(file, expr->lhs, &la, &lb)) {
      return false;
    }
    *a = -la;
    *b = -lb;
    return true;
  case CAL_EXPR_CALL:
    /* The callee's line composed with the argument's: c(a x + b) + d. */
    if (!affine(file, expr->lhs, &la, &lb) || !affine(file, callee_of(file, expr)->body, &ra, &rb)) {
      return false;
    }
    *a = ra * la;
    *b = ra * lb + rb;
    return true;
  case CAL_EXPR_BINARY:
    if (!affine(file, expr->lhs, &la, &lb) || !affine(file, expr->rhs, &ra, &rb)) {
      return false;
    }
    switch (expr->op) {
    case '+':
      *a = la + ra;
      *b = lb + rb;
      return true;
    case '-':
      *a = la - ra;
      *b = lb - rb;
      return true;
    case '*':
      /* A product is a line only when one side is a constant. */
      if (la == 0) {
        *a = lb * ra;
        *b = lb * rb;
        return true;
      }
      if (ra == 0) {
        *a = la * rb;
        *b = lb * rb;
        return true;
      }
      return false;
    default:
      if (ra != 0 || rb == 0) {
        return false;
      }
      *a = la / rb;
      *b = lb / rb;
      return true;
    }
  }
  return false;
}

/* ---- Emitting code --------------------------------------------------------------------------- */

typedef struct {
  CalInstruction *code;
  size_t length;
  size_t capacity;
  /* The first problem, as a message; emission stops caring about the rest. */
  const char *problem;
} Builder;

static void emit(Builder *builder, CalOp op, uint32_t reg, double literal) {
  if (builder->problem != NULL) {
    return;
  }
  if (builder->length >= CAL_CODE_MAX) {
    builder->problem = "inlines to more instructions than this build allows";
    return;
  }
  if (builder->length == builder->capacity) {
    size_t capacity = builder->capacity == 0 ? 16 : builder->capacity * 2;
    CalInstruction *grown = realloc(builder->code, capacity * sizeof(*grown));
    if (grown == NULL) {
      builder->problem = "out of memory";
      return;
    }
    builder->code = grown;
    builder->capacity = capacity;
  }
  builder->code[builder->length++] = (CalInstruction){ .op = op, .reg = reg, .literal = literal };
}

/* `reg` is the register holding the current function's argument: 0 at the top, one deeper per call
 * being inlined. */
static void emit_expr(Builder *builder, const AstFile *file, const CalExpr *expr, uint32_t reg) {
  if (!depends(file, expr)) {
    double value = tree_eval(file, expr, 0);
    if (!isfinite(value)) {
      builder->problem = "has a constant part that is not a finite number";
      return;
    }
    emit(builder, CAL_OP_LIT, 0, value);
    return;
  }

  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    emit(builder, CAL_OP_LIT, 0, expr->number);
    return;
  case CAL_EXPR_NAME:
    emit(builder, CAL_OP_LOAD, reg, 0);
    return;
  case CAL_EXPR_NEGATE:
    emit_expr(builder, file, expr->lhs, reg);
    emit(builder, CAL_OP_NEG, 0, 0);
    return;
  case CAL_EXPR_CALL:
    if (reg + 1 >= CAL_REGISTER_MAX) {
      builder->problem = "nests calls more deeply than this build allows";
      return;
    }
    emit_expr(builder, file, expr->lhs, reg);
    emit(builder, CAL_OP_STORE, reg + 1, 0);
    emit_expr(builder, file, callee_of(file, expr)->body, reg + 1);
    return;
  case CAL_EXPR_BINARY:
    emit_expr(builder, file, expr->lhs, reg);
    emit_expr(builder, file, expr->rhs, reg);
    switch (expr->op) {
    case '+':
      emit(builder, CAL_OP_ADD, 0, 0);
      break;
    case '-':
      emit(builder, CAL_OP_SUB, 0, 0);
      break;
    case '*':
      emit(builder, CAL_OP_MUL, 0, 0);
      break;
    default:
      emit(builder, CAL_OP_DIV, 0, 0);
      break;
    }
    return;
  }
}

static uint32_t stack_depth(const CalInstruction *code, size_t length) {
  uint32_t depth = 0;
  uint32_t deepest = 0;
  for (size_t i = 0; i < length; ++i) {
    switch (code[i].op) {
    case CAL_OP_LOAD:
    case CAL_OP_LIT:
      depth += 1;
      break;
    case CAL_OP_STORE:
    case CAL_OP_ADD:
    case CAL_OP_SUB:
    case CAL_OP_MUL:
    case CAL_OP_DIV:
      depth -= 1;
      break;
    case CAL_OP_NEG:
      break;
    }
    if (depth > deepest) {
      deepest = depth;
    }
  }
  return deepest;
}

/* One direction, into `function` and, when it is code, into `builder`. The code pointer is fixed up
 * once everything has been copied into the final block. */
static bool compile_function(
  const AstFile *file,
  const AstCal *cal,
  const CalExpr *body,
  CalFunction *function,
  Builder *builder,
  DiagList *diags
) {
  double a;
  double b;
  if (affine(file, body, &a, &b)) {
    if (!isfinite(a) || !isfinite(b)) {
      diag_add(diags, cal->line, cal->column, 0, "calibration '%s' folds to a line that is not finite", cal->name);
      return false;
    }
    *function = (CalFunction){ .linear = true, .scale = a, .offset = b };
    return true;
  }

  size_t start = builder->length;
  emit_expr(builder, file, body, 0);
  if (builder->problem == NULL && stack_depth(builder->code + start, builder->length - start) > CAL_STACK_MAX) {
    builder->problem = "needs a deeper stack than this build allows";
  }
  if (builder->problem != NULL) {
    diag_add(diags, cal->line, cal->column, 0, "calibration '%s' %s", cal->name, builder->problem);
    return false;
  }

  /* An offset into the pool for now; a pointer once the pool has its final address. */
  *function = (CalFunction){ .linear = false,
                             .code = (const CalInstruction *)(uintptr_t)start,
                             .length = (uint32_t)(builder->length - start) };
  return true;
}

static size_t round_up(size_t value, size_t alignment) {
  size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

CalTable *cal_compile(const AstFile *file, DiagList *diags) {
  CalCompiled *cals = calloc(file->cal_count == 0 ? 1 : file->cal_count, sizeof(*cals));
  Builder builder = { 0 };
  if (cals == NULL) {
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  bool ok = true;
  for (size_t i = 0; i < file->cal_count && ok; ++i) {
    const AstCal *cal = &file->cals[i];
    CalCompiled *compiled = &cals[i];
    compiled->name = cal->name;

    ok = compile_function(file, cal, cal->body, &compiled->forward, &builder, diags);
    if (!ok) {
      break;
    }

    /* An inverse somebody wrote wins, even over a line: they may know something about rounding that
     * solving the line does not. */
    if (cal->inverse != NULL) {
      ok = compile_function(file, cal, cal->inverse, &compiled->inverse, &builder, diags);
      compiled->invertible = ok;
    } else if (compiled->forward.linear && compiled->forward.scale != 0) {
      compiled->invertible = true;
      compiled->inverse_solves_line = true;
    }
  }

  if (!ok) {
    free(builder.code);
    free(cals);
    return NULL;
  }

  /* One block: the table, the calibrations, and the code they share. */
  size_t cals_offset = round_up(sizeof(CalTable), alignof(CalCompiled));
  size_t code_offset = round_up(cals_offset + file->cal_count * sizeof(CalCompiled), alignof(CalInstruction));
  size_t total = code_offset + builder.length * sizeof(CalInstruction);

  uint8_t *block = calloc(1, total);
  if (block == NULL) {
    free(builder.code);
    free(cals);
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  CalTable *table = (CalTable *)block;
  CalCompiled *placed = (CalCompiled *)(block + cals_offset);
  CalInstruction *pool = (CalInstruction *)(block + code_offset);

  if (builder.length > 0) {
    memcpy(pool, builder.code, builder.length * sizeof(*pool));
  }
  for (size_t i = 0; i < file->cal_count; ++i) {
    placed[i] = cals[i];
    if (!placed[i].forward.linear) {
      placed[i].forward.code = pool + (uintptr_t)cals[i].forward.code;
    }
    if (placed[i].invertible && !placed[i].inverse_solves_line && !placed[i].inverse.linear) {
      placed[i].inverse.code = pool + (uintptr_t)cals[i].inverse.code;
    }
  }

  table->cals = placed;
  table->count = file->cal_count;

  free(builder.code);
  free(cals);
  return table;
}

void cal_table_free(CalTable *table) {
  free(table);
}

/* ---- Running ---------------------------------------------------------------------------------- */

bool cal_eval(const CalFunction *function, double x, double *out) {
  double result;

  if (function->linear) {
    result = function->scale * x + function->offset;
  } else {
    double stack[CAL_STACK_MAX];
    double regs[CAL_REGISTER_MAX];
    size_t depth = 0;
    regs[0] = x;

    /* The depth and the registers were bounded at load time, so this cannot overrun them. */
    for (uint32_t i = 0; i < function->length; ++i) {
      const CalInstruction *instruction = &function->code[i];
      switch (instruction->op) {
      case CAL_OP_LOAD:
        stack[depth++] = regs[instruction->reg];
        break;
      case CAL_OP_STORE:
        regs[instruction->reg] = stack[--depth];
        break;
      case CAL_OP_LIT:
        stack[depth++] = instruction->literal;
        break;
      case CAL_OP_NEG:
        stack[depth - 1] = -stack[depth - 1];
        break;
      case CAL_OP_ADD:
        depth -= 1;
        stack[depth - 1] = stack[depth - 1] + stack[depth];
        break;
      case CAL_OP_SUB:
        depth -= 1;
        stack[depth - 1] = stack[depth - 1] - stack[depth];
        break;
      case CAL_OP_MUL:
        depth -= 1;
        stack[depth - 1] = stack[depth - 1] * stack[depth];
        break;
      case CAL_OP_DIV:
        depth -= 1;
        stack[depth - 1] = stack[depth - 1] / stack[depth];
        break;
      }
    }
    result = stack[0];
  }

  if (!isfinite(result)) {
    return false;
  }
  *out = result;
  return true;
}

bool cal_forward(const CalCompiled *cal, double raw, double *out) {
  return cal_eval(&cal->forward, raw, out);
}

bool cal_backward(const CalCompiled *cal, double eng, double *out) {
  if (!cal->invertible) {
    return false;
  }
  if (!cal->inverse_solves_line) {
    return cal_eval(&cal->inverse, eng, out);
  }

  double result = (eng - cal->forward.offset) / cal->forward.scale;
  if (!isfinite(result)) {
    return false;
  }
  *out = result;
  return true;
}

/* ---- Emitting C -------------------------------------------------------------------------------
 *
 * Generated from the compiled form, not the source, so it is the same arithmetic in the same order:
 * a line is `scale * x + offset` with the folded constants, and code is the postfix program turned
 * back into fully parenthesised infix. Every literal is printed with 17 significant digits, which is
 * enough for a double to read back as exactly itself. */

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
  bool failed;
} Text;

static void append(Text *text, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void append(Text *text, const char *fmt, ...) {
  if (text->failed) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  va_list again;
  va_copy(again, args);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  if (needed < 0) {
    text->failed = true;
    va_end(again);
    return;
  }
  if (text->length + (size_t)needed + 1 > text->capacity) {
    size_t capacity = text->capacity == 0 ? 256 : text->capacity;
    while (text->length + (size_t)needed + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(text->text, capacity);
    if (grown == NULL) {
      text->failed = true;
      va_end(again);
      return;
    }
    text->text = grown;
    text->capacity = capacity;
  }
  vsnprintf(text->text + text->length, text->capacity - text->length, fmt, again);
  text->length += (size_t)needed;
  va_end(again);
}

/* A double as a C literal that reads back as exactly itself, and is a double rather than an int. */
static void literal(char *out, size_t capacity, double value) {
  char digits[40];
  snprintf(digits, sizeof(digits), "%.17g", value);
  bool looks_real = strpbrk(digits, ".en") != NULL;
  snprintf(out, capacity, value < 0 ? "(%s%s)" : "%s%s", digits, looks_real ? "" : ".0");
}

/* The postfix program as statements and one returned expression. `in` is the argument's name. */
static void emit_c_code(Text *text, const CalFunction *function, const char *in) {
  char *stack[CAL_STACK_MAX] = { 0 };
  size_t depth = 0;
  /* Which temporary holds each register, since a register is reused by every call at its depth. */
  unsigned names[CAL_REGISTER_MAX] = { 0 };
  unsigned next_name = 1;

  for (uint32_t i = 0; i < function->length && !text->failed; ++i) {
    const CalInstruction *instruction = &function->code[i];
    char buffer[64];
    Text item = { 0 };

    switch (instruction->op) {
    case CAL_OP_LOAD:
      if (instruction->reg == 0) {
        append(&item, "%s", in);
      } else {
        append(&item, "t%u", names[instruction->reg]);
      }
      stack[depth++] = item.text;
      continue;
    case CAL_OP_LIT:
      literal(buffer, sizeof(buffer), instruction->literal);
      append(&item, "%s", buffer);
      stack[depth++] = item.text;
      continue;
    case CAL_OP_STORE:
      names[instruction->reg] = next_name++;
      depth -= 1;
      append(text, "  const double t%u = %s;\n", names[instruction->reg], stack[depth]);
      free(stack[depth]);
      continue;
    case CAL_OP_NEG:
      append(&item, "(-%s)", stack[depth - 1]);
      free(stack[depth - 1]);
      stack[depth - 1] = item.text;
      continue;
    default: {
      const char op = instruction->op == CAL_OP_ADD   ? '+'
                      : instruction->op == CAL_OP_SUB ? '-'
                      : instruction->op == CAL_OP_MUL ? '*'
                                                      : '/';
      depth -= 1;
      append(&item, "(%s %c %s)", stack[depth - 1], op, stack[depth]);
      free(stack[depth - 1]);
      free(stack[depth]);
      stack[depth - 1] = item.text;
      continue;
    }
    }
  }

  if (depth > 0 && stack[0] != NULL) {
    append(text, "  return %s;\n", stack[0]);
  } else {
    text->failed = true;
  }
  for (size_t i = 0; i < depth; ++i) {
    free(stack[i]);
  }
}

static void emit_c_function(
  Text *text,
  const char *prefix,
  const char *name,
  const CalFunction *function,
  const char *in
) {
  append(text, "static inline double %s%s(double %s) {\n", prefix, name, in);
  if (function->linear) {
    char scale[64];
    char offset[64];
    literal(scale, sizeof(scale), function->scale);
    literal(offset, sizeof(offset), function->offset);
    append(text, "  return %s * %s + %s;\n", scale, in, offset);
  } else {
    emit_c_code(text, function, in);
  }
  append(text, "}\n\n");
}

/* An imported declaration is called `vendor.celsius`; in C it is `vendor_celsius`. */
static void c_identifier(char *out, size_t capacity, const char *name) {
  snprintf(out, capacity, "%s", name);
  for (char *dot = strchr(out, '.'); dot != NULL; dot = strchr(dot, '.')) {
    *dot = '_';
  }
}

/* One enum (#11): its names as constants, and a function from a value to its name. A C enum when
 * every value fits an int, which is what an enumeration constant is; `#define`s otherwise, since a
 * 64-bit field's names may not. Either way a constant is usable in a `case`. */
static void emit_c_enum(Text *text, const char *prefix, const AstEnum *enumeration) {
  char c_name[256];
  c_identifier(c_name, sizeof(c_name), enumeration->name);

  bool fits_int = true;
  for (size_t i = 0; i < enumeration->item_count; ++i) {
    fits_int = fits_int && enumeration->items[i].value <= 0x7FFFFFFF;
  }

  append(text, "/* enum %s: %llu bits */\n", enumeration->name, (unsigned long long)enumeration->width);
  if (fits_int) {
    append(text, "enum %s%s {\n", prefix, c_name);
    for (size_t i = 0; i < enumeration->item_count; ++i) {
      const AstEnumItem *item = &enumeration->items[i];
      append(text, "  %s%s_%s = %llu,\n", prefix, c_name, item->name, (unsigned long long)item->value);
    }
    append(text, "};\n\n");
  } else {
    for (size_t i = 0; i < enumeration->item_count; ++i) {
      const AstEnumItem *item = &enumeration->items[i];
      append(text, "#define %s%s_%s %lluULL\n", prefix, c_name, item->name, (unsigned long long)item->value);
    }
    append(text, "\n");
  }

  append(text, "static inline const char *%s%s_name(unsigned long long value) {\n", prefix, c_name);
  append(text, "  switch (value) {\n");
  for (size_t i = 0; i < enumeration->item_count; ++i) {
    const AstEnumItem *item = &enumeration->items[i];
    append(text, "  case %lluULL:\n    return \"%s\";\n", (unsigned long long)item->value, item->name);
  }
  append(text, "  default:\n    return (const char *)0;\n  }\n}\n\n");
}

size_t cal_emit_c(const CalTable *table, const AstFile *file, const char *prefix, char *out, size_t capacity) {
  Text text = { 0 };

  append(
    &text,
    "/* Generated by frame-dsl --emit-c. Do not edit: change the .fdsl and generate it again.\n"
    " *\n"
    " * Each function computes exactly what the library computes for the same calibration, provided\n"
    " * the compiler keeps the arithmetic as written: no -ffast-math, and no contraction into fused\n"
    " * multiply-adds (-ffp-contract=off, which is already the default under -std=c11). */\n"
    "#pragma once\n\n"
  );

  for (size_t i = 0; i < table->count; ++i) {
    const CalCompiled *cal = &table->cals[i];

    char c_name[256];
    c_identifier(c_name, sizeof(c_name), cal->name);

    emit_c_function(&text, prefix, c_name, &cal->forward, "raw");

    if (!cal->invertible) {
      append(&text, "/* %s%s has no inverse: it is not linear and declares none. */\n\n", prefix, c_name);
      continue;
    }

    char inverse_name[sizeof(c_name) + sizeof("_inverse")];
    snprintf(inverse_name, sizeof(inverse_name), "%s_inverse", c_name);
    if (cal->inverse_solves_line) {
      char scale[64];
      char offset[64];
      literal(scale, sizeof(scale), cal->forward.scale);
      literal(offset, sizeof(offset), cal->forward.offset);
      append(&text, "static inline double %s%s(double eng) {\n", prefix, inverse_name);
      append(&text, "  return (eng - %s) / %s;\n}\n\n", offset, scale);
    } else {
      emit_c_function(&text, prefix, inverse_name, &cal->inverse, "eng");
    }
  }

  for (size_t i = 0; file != NULL && i < file->enum_count; ++i) {
    emit_c_enum(&text, prefix, &file->enums[i]);
  }

  size_t length = text.failed ? 0 : text.length;
  if (capacity > 0 && out != NULL) {
    size_t copied = length < capacity - 1 ? length : capacity - 1;
    if (copied > 0) {
      memcpy(out, text.text, copied);
    }
    out[copied] = '\0';
  }
  free(text.text);
  return length;
}
