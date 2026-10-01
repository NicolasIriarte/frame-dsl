#include "bytecode.h"

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

/* A width nothing sane reaches, matching the ceiling the checker folds against. Kept here as well so
 * a per-frame evaluation cannot run away on a corrupt length field. */
#define WIDTH_CEILING ((uint64_t)1 << 32)

/* ---- Measuring ------------------------------------------------------------------------------
 *
 * Compilation runs twice over the same tree: once to count instructions, once to emit them. That is
 * what lets the whole program be one allocation -- the alternative is growing an array per packet and
 * ending up with an allocation count that depends on the input. */

static bool uses_valof(const Expr *expr) {
  if (expr == NULL) {
    return false;
  }
  if (expr->kind == EXPR_VALOF) {
    return true;
  }
  return expr->kind == EXPR_BINARY && (uses_valof(expr->lhs) || uses_valof(expr->rhs));
}

/* Folds a subtree with no valof() in it. The checker has already rejected anything that overflows or
 * divides by zero, so this only has to agree with it, not re-report it. */
static uint64_t fold(const Expr *expr) {
  switch (expr->kind) {
  case EXPR_LITERAL:
    return expr->literal;
  case EXPR_VALOF:
    return 0; /* unreachable: callers check uses_valof() first */
  case EXPR_BINARY: {
    uint64_t lhs = fold(expr->lhs);
    uint64_t rhs = fold(expr->rhs);
    switch (expr->op) {
    case '+':
      return lhs + rhs;
    case '-':
      return lhs - rhs;
    case '*':
      return lhs * rhs;
    case '/':
      return rhs == 0 ? 0 : lhs / rhs;
    default:
      return 0;
    }
  }
  }
  return 0;
}

/* Instructions a subtree compiles to, with constant subtrees counting as one.
 *
 * Folding them is worth the two lines: `(valof(n) + 1) * 8` has to stay four instructions, but
 * `valof(n) * (4 * 2)` becomes three, and the saving is paid per field per frame rather than once. */
static size_t measure(const Expr *expr) {
  if (expr == NULL || !uses_valof(expr)) {
    return 1;
  }
  if (expr->kind == EXPR_BINARY) {
    return measure(expr->lhs) + measure(expr->rhs) + 1;
  }
  return 1;
}

/* ---- Emitting -------------------------------------------------------------------------------- */

typedef struct {
  WidthInstruction *code;
  size_t length;
} Emitter;

static void emit(Emitter *emitter, WidthOp op, uint64_t operand) {
  emitter->code[emitter->length++] = (WidthInstruction){ .op = op, .operand = operand };
}

/* The field index a valof() names. Check 7 has already established it exists and is earlier, so a
 * miss here would be a bug rather than bad input. */
static uint64_t slot_of(const AstPacket *packet, const char *name) {
  for (size_t i = 0; i < packet->field_count; ++i) {
    if (strcmp(packet->fields[i].name, name) == 0) {
      return i;
    }
  }
  return 0;
}

static void compile_expr(Emitter *emitter, const AstPacket *packet, const Expr *expr) {
  if (!uses_valof(expr)) {
    emit(emitter, OP_LIT, fold(expr));
    return;
  }

  if (expr->kind == EXPR_VALOF) {
    emit(emitter, OP_SLOT, slot_of(packet, expr->reference));
    return;
  }

  compile_expr(emitter, packet, expr->lhs);
  compile_expr(emitter, packet, expr->rhs);
  switch (expr->op) {
  case '+':
    emit(emitter, OP_ADD, 0);
    break;
  case '-':
    emit(emitter, OP_SUB, 0);
    break;
  case '*':
    emit(emitter, OP_MUL, 0);
    break;
  default:
    emit(emitter, OP_DIV, 0);
    break;
  }
}

/* The deepest the operand stack gets. Walking the emitted code rather than the tree, because the code
 * is what the evaluator runs -- a depth derived from the tree would be a second opinion about a
 * different thing. */
static uint32_t stack_depth(const WidthInstruction *code, size_t length) {
  uint32_t depth = 0;
  uint32_t deepest = 0;
  for (size_t i = 0; i < length; ++i) {
    if (code[i].op == OP_SLOT || code[i].op == OP_LIT) {
      depth += 1;
      if (depth > deepest) {
        deepest = depth;
      }
    } else if (depth >= 2) {
      depth -= 1;
    }
  }
  return deepest;
}

/* ---- Compilation ----------------------------------------------------------------------------- */

/* The calibration a field names, by index. Check 18 guarantees it exists. */
static int32_t cal_index(const AstFile *file, const char *name) {
  if (name == NULL) {
    return -1;
  }
  for (size_t i = 0; i < file->cal_count; ++i) {
    if (strcmp(file->cals[i].name, name) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

/* The enum a field is typed by, by index. Composition resolved it by the same name. */
static int32_t enum_index(const AstFile *file, const char *name) {
  if (name == NULL) {
    return -1;
  }
  for (size_t i = 0; i < file->enum_count; ++i) {
    if (strcmp(file->enums[i].name, name) == 0) {
      return (int32_t)i;
    }
  }
  return -1;
}

static size_t round_up(size_t value, size_t alignment) {
  size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

/* ---- Repeated blocks (#55) ----------------------------------------------------------------------
 *
 * A repetition is walked as one bit string; what is inside it only matters to the accessors, and to the
 * encoder for the fixed values an element carries. So each repeated block is laid out once here, apart
 * from the packets, and every repetition of it points at the same element. */

static const ElementProgram *element_of(const Program *program, const AstPacket *block) {
  for (size_t i = 0; i < program->element_count; ++i) {
    if (program->elements[i].block == block) {
      return &program->elements[i];
    }
  }
  return NULL;
}

static bool compile_element(ElementProgram *element, const AstFile *file, const AstPacket *block) {
  element->block = block;
  element->member_count = block->field_count;
  element->members = calloc(block->field_count == 0 ? 1 : block->field_count, sizeof(MemberInstruction));
  if (element->members == NULL) {
    return false;
  }
  uint64_t offset = 0;
  for (size_t f = 0; f < block->field_count; ++f) {
    const AstField *field = &block->fields[f];
    /* Constant, and checked sound: composition refuses to repeat a block with any other width. */
    uint64_t width = fold(field->width);
    element->members[f] = (MemberInstruction){
      .name = field->name,
      .offset_bits = offset,
      .width_bits = width,
      .is_scalar = width <= FRAMEDSL_SCALAR_MAX_BITS && field->element == NULL,
      .has_fixed = field->has_fixed,
      .fixed = field->fixed,
      .cal = cal_index(file, field->cal),
      .enumeration = enum_index(file, field->enumeration),
      .bit_order = field->bit_order,
      .byte_order = field->byte_order,
    };
    offset += width;
  }
  element->bits = offset;
  return true;
}

static bool compile_elements(Program *program, const AstFile *file) {
  size_t capacity = 0;
  for (size_t p = 0; p < file->packet_count; ++p) {
    for (size_t f = 0; f < file->packets[p].field_count; ++f) {
      const AstPacket *block = file->packets[p].fields[f].element;
      if (block == NULL || element_of(program, block) != NULL) {
        continue;
      }
      if (program->element_count == capacity) {
        capacity = capacity == 0 ? 4 : capacity * 2;
        ElementProgram *grown = realloc(program->elements, capacity * sizeof(*grown));
        if (grown == NULL) {
          return false;
        }
        program->elements = grown;
      }
      ElementProgram *element = &program->elements[program->element_count];
      *element = (ElementProgram){ 0 };
      program->element_count += 1;
      if (!compile_element(element, file, block)) {
        return false;
      }
    }
  }
  return true;
}

Program *program_compile(const AstFile *file, const Layout *layout, DiagList *diags) {
  if (file == NULL || layout == NULL) {
    return NULL;
  }

  size_t field_total = 0;
  size_t code_total = 0;
  for (size_t p = 0; p < file->packet_count; ++p) {
    const AstPacket *packet = &file->packets[p];
    field_total += packet->field_count;
    for (size_t f = 0; f < packet->field_count; ++f) {
      if (layout->packets[p].fields[f].variable) {
        code_total += measure(packet->fields[f].width);
      }
    }
  }

  size_t packets_offset = round_up(sizeof(Program), alignof(PacketProgram));
  size_t fields_offset =
    round_up(packets_offset + file->packet_count * sizeof(PacketProgram), alignof(FieldInstruction));
  size_t code_offset = round_up(fields_offset + field_total * sizeof(FieldInstruction), alignof(WidthInstruction));
  size_t total = code_offset + code_total * sizeof(WidthInstruction);

  uint8_t *block = calloc(1, total);
  if (block == NULL) {
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  Program *program = (Program *)block;
  PacketProgram *packets = (PacketProgram *)(block + packets_offset);
  FieldInstruction *fields = (FieldInstruction *)(block + fields_offset);
  Emitter emitter = { .code = (WidthInstruction *)(block + code_offset), .length = 0 };

  program->packets = packets;
  program->packet_count = file->packet_count;
  if (!compile_elements(program, file)) {
    diag_add(diags, 1, 1, 0, "out of memory");
    program_free(program);
    return NULL;
  }

  size_t next_field = 0;
  for (size_t p = 0; p < file->packet_count; ++p) {
    const AstPacket *packet = &file->packets[p];
    const PacketLayout *packet_layout = &layout->packets[p];

    packets[p].fields = &fields[next_field];
    packets[p].field_count = packet->field_count;

    for (size_t f = 0; f < packet->field_count; ++f) {
      const AstField *field = &packet->fields[f];
      const FieldLayout *slot = &packet_layout->fields[f];
      FieldInstruction *instruction = &fields[next_field++];

      *instruction = (FieldInstruction){
        .field = (uint32_t)f,
        .variable = slot->variable,
        .width_bits = slot->variable ? 0 : slot->width_bits,
        .max_bits = slot->variable ? slot->max_bits : slot->width_bits,
        .is_scalar = slot->is_scalar,
        .has_fixed = field->has_fixed,
        .fixed = field->has_fixed ? field->fixed : 0,
        .cal = cal_index(file, field->cal),
        .enumeration = enum_index(file, field->enumeration),
        .bit_order = field->bit_order,
        .byte_order = field->byte_order,
        .element = field->element != NULL ? element_of(program, field->element) : NULL,
      };

      if (!slot->variable) {
        continue;
      }

      size_t start = emitter.length;
      compile_expr(&emitter, packet, field->width);

      instruction->width.code = &emitter.code[start];
      instruction->width.length = (uint32_t)(emitter.length - start);
      instruction->width.stack_depth = stack_depth(instruction->width.code, instruction->width.length);

      if (instruction->width.stack_depth > WIDTH_STACK_MAX) {
        diag_add(
          diags,
          field->line,
          field->column,
          0,
          "'%s.%s' has a width expression nested too deeply to evaluate; this build allows a "
          "depth of %d",
          packet->name,
          field->name,
          WIDTH_STACK_MAX
        );
        free(block);
        return NULL;
      }
    }
  }

  return program;
}

void program_free(Program *program) {
  if (program == NULL) {
    return;
  }
  for (size_t i = 0; i < program->element_count; ++i) {
    free(program->elements[i].members);
  }
  free(program->elements);
  /* The program *is* the block, so this frees the instructions and the code with it. */
  free(program);
}

bool width_program_eval(const WidthProgram *program, const uint64_t *slots, uint64_t *out) {
  if (program == NULL || program->code == NULL || slots == NULL || out == NULL) {
    return false;
  }

  uint64_t stack[WIDTH_STACK_MAX];
  size_t depth = 0;

  for (uint32_t i = 0; i < program->length; ++i) {
    const WidthInstruction *instruction = &program->code[i];

    switch (instruction->op) {
    case OP_SLOT:
      stack[depth++] = slots[instruction->operand];
      break;
    case OP_LIT:
      stack[depth++] = instruction->operand;
      break;
    default: {
      /* The depth was computed at compile time, so a well-formed program cannot underflow here.
       * Checked anyway: the cost is one comparison per operator, and the alternative is that a bug in
       * codegen reads off the bottom of this stack rather than failing. */
      if (depth < 2) {
        return false;
      }
      uint64_t rhs = stack[--depth];
      uint64_t lhs = stack[depth - 1];

      switch (instruction->op) {
      case OP_ADD:
        lhs += rhs;
        break;
      case OP_SUB:
        /* Unsigned, so a width that would go negative wraps rather than being caught by a sign. A
         * length field claiming less than the header it counts is exactly how that happens. */
        if (rhs > lhs) {
          return false;
        }
        lhs -= rhs;
        break;
      case OP_MUL:
        if (lhs != 0 && rhs > WIDTH_CEILING / lhs) {
          return false;
        }
        lhs *= rhs;
        break;
      default:
        if (rhs == 0) {
          return false;
        }
        lhs /= rhs;
        break;
      }

      if (lhs > WIDTH_CEILING) {
        return false;
      }
      stack[depth - 1] = lhs;
      break;
    }
    }
  }

  if (depth != 1) {
    return false;
  }
  *out = stack[0];
  return true;
}
