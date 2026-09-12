/* Story 8: compiling the checked model to bytecode.
 *
 * Tested against the compiled form and the evaluator directly, because there is no encoder yet and
 * there will not be one until #25. That is the point of doing it this way round: when the encoder
 * arrives, "the bytes came out right" and "the width program is right" are two separate claims, and
 * a bug in one cannot be hidden by a bug in the other.
 *
 * Instructions are compared by what they say, not by counting them -- except where the count is the
 * claim, as in the case that a constant width compiles to no program at all. */
#include <stdio.h>
#include <string.h>

#include "bytecode.h"
#include "check.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

typedef struct {
  DiagList diags;
  AstFile *file;
  Layout *layout;
  Program *program;
} Compiled;

static bool compile_source(const char *source, Compiled *out) {
  diag_init(&out->diags);
  out->file = parse(source, &out->diags);
  out->layout = NULL;
  out->program = NULL;

  if (out->diags.count != 0 || out->file == NULL) {
    return false;
  }
  out->layout = check_file(out->file, &out->diags);
  if (out->layout == NULL) {
    return false;
  }
  out->program = program_compile(out->file, out->layout, &out->diags);
  return out->program != NULL;
}

static void compiled_free(Compiled *compiled) {
  program_free(compiled->program);
  layout_free(compiled->layout);
  ast_free(compiled->file);
  diag_free(&compiled->diags);
}

static bool instruction_is(const WidthInstruction *instruction, WidthOp op, uint64_t operand) {
  return instruction->op == op && instruction->operand == operand;
}

/* One instruction per field, in declaration order, carrying the width the layout worked out. */
static int us8_tc01_a_constant_packet_compiles_field_by_field(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; a: 11; b: 1; c: 4; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const PacketProgram *packet = &compiled.program->packets[0];
  const uint64_t widths[] = {8, 11, 1, 4};

  if (compiled.program->packet_count != 1 || packet->field_count != 4) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < 4; ++i) {
      if (packet->fields[i].field != i || packet->fields[i].width_bits != widths[i] ||
          packet->fields[i].variable) {
        failure = __LINE__;
        break;
      }
    }
  }

  compiled_free(&compiled);
  return failure;
}

/* A constant width is a number on the instruction, not a one-instruction program: the common case
 * costs the interpreter no evaluation at all. */
static int us8_tc02_a_constant_width_compiles_to_no_program(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; a: 4 * 4; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const FieldInstruction *a = &compiled.program->packets[0].fields[1];
  if (a->width.code != NULL || a->width.length != 0) {
    failure = __LINE__;
  } else if (a->width_bits != 16) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

static int us8_tc03_a_valof_width_compiles_to_postfix(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 16; d[8]: valof(n) * 8; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const FieldInstruction *d = &compiled.program->packets[0].fields[2];

  if (!d->variable || d->width.length != 3) {
    failure = __LINE__;
  } else if (!instruction_is(&d->width.code[0], OP_SLOT, 1)) {
    failure = __LINE__; /* `n` is field 1 of this packet */
  } else if (!instruction_is(&d->width.code[1], OP_LIT, 8)) {
    failure = __LINE__;
  } else if (!instruction_is(&d->width.code[2], OP_MUL, 0)) {
    failure = __LINE__;
  } else if (d->width.stack_depth != 2) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

/* A subtree with no valof() in it is worth one instruction rather than three. The saving is paid per
 * field per frame; the folding is done once. */
static int us8_tc04_constant_subtrees_are_folded(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 16; d[8]: valof(n) * (4 * 2); };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const FieldInstruction *d = &compiled.program->packets[0].fields[2];

  if (d->width.length != 3) {
    failure = __LINE__;
  } else if (!instruction_is(&d->width.code[1], OP_LIT, 8)) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

/* Evaluation order, checked by evaluating rather than by reading the instructions back -- an
 * assertion about the instruction list would restate the compiler to itself. */
static int us8_tc05_a_nested_expression_evaluates_correctly(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 16; d[512]: (valof(n) + 1) * 8; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const uint64_t slots[] = {0, 9, 0};
  uint64_t width = 0;

  if (!width_program_eval(&compiled.program->packets[0].fields[2].width, slots, &width)) {
    failure = __LINE__;
  } else if (width != 80) {
    failure = __LINE__; /* (9 + 1) * 8, not 9 + (1 * 8) */
  }

  compiled_free(&compiled);
  return failure;
}

static int us8_tc06_every_operator_evaluates(const TestArguments *args) {
  (void)args;

  /* Every width here is a provable byte multiple, because check 13 rejects one that is not before
   * codegen ever sees it -- `valof(n) + 8` does not compile, and cannot be a fixture. */
  const struct {
    const char *width;
    uint64_t expected;
  } cases[] = {
      {"valof(n) * 8 + 16", 656},
      {"valof(n) * 8 - 16", 624},
      {"valof(n) * 8", 640},
      {"valof(n) / 8 * 8", 80},
  };

  int failure = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases) && failure == 0; ++i) {
    char source[256];
    snprintf(source, sizeof(source), "pkt A { h: 8 = 0xAA; n: 16; d[512]: %s; };", cases[i].width);

    Compiled compiled;
    if (!compile_source(source, &compiled)) {
      compiled_free(&compiled);
      FORCE_FAILURE();
    }

    const uint64_t slots[] = {0, 80, 0};
    uint64_t width = 0;
    if (!width_program_eval(&compiled.program->packets[0].fields[2].width, slots, &width)) {
      failure = __LINE__;
    } else if (width != cases[i].expected) {
      failure = __LINE__;
    }
    compiled_free(&compiled);
  }
  return failure;
}

/* The depth the evaluator relies on, checked against a shape whose true maximum is not the same as
 * its instruction count. */
static int us8_tc07_the_stack_depth_is_the_true_maximum(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 16; d[512]: valof(n) * 8 + valof(n) * 8; };",
                      &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const WidthProgram *program = &compiled.program->packets[0].fields[2].width;

  /* SLOT LIT MUL SLOT LIT MUL ADD: seven instructions, and the stack never holds more than three. */
  if (program->length != 7) {
    failure = __LINE__;
  } else if (program->stack_depth != 3) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

/* Both of these depend on a frame's contents, so neither can be settled when the file loads. The
 * evaluator says no rather than returning a width nothing could hold. */
static int us8_tc08_a_runaway_width_is_refused(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 64; d[8]: valof(n) * 8; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const WidthProgram *program = &compiled.program->packets[0].fields[2].width;
  const uint64_t huge[] = {0, UINT64_MAX, 0};
  uint64_t width = 0;

  if (width_program_eval(program, huge, &width)) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  if (failure != 0) {
    return failure;
  }

  /* And a subtraction that would go below zero, which is what a length field claiming less than the
   * header it counts produces. */
  Compiled second;
  if (!compile_source("pkt A { h: 8 = 0xAA; n: 16; d[512]: valof(n) * 8 - 32; };", &second)) {
    compiled_free(&second);
    FORCE_FAILURE();
  }

  const uint64_t small[] = {0, 1, 0};
  if (width_program_eval(&second.program->packets[0].fields[2].width, small, &width)) {
    failure = __LINE__; /* 1 * 8 - 32 */
  }

  compiled_free(&second);
  return failure;
}

/* What the encoder will write from, rather than from the arena, so a caller cannot emit a frame that
 * fails to identify itself. */
static int us8_tc09_a_fixed_field_carries_its_value(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { h: 8 = 0xAA; a: 8; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  const PacketProgram *packet = &compiled.program->packets[0];
  if (!packet->fields[0].has_fixed || packet->fields[0].fixed != 0xAA) {
    failure = __LINE__;
  } else if (packet->fields[1].has_fixed) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

/* Resolved, not written: a field inheriting an order from a block and one declaring it compile to the
 * same instruction, because the runtime cares how the bits land and not which scope said so. */
static int us8_tc10_orders_are_carried_resolved(const TestArguments *args) {
  (void)args;

  Compiled inherited;
  Compiled written;
  bool ok = compile_source("pkt A { h: 8 = 0xAA;\n"
                           "  [ @byte_order: little_endian; ]\n"
                           "  block { a: 32; };\n"
                           "};",
                           &inherited) &&
            compile_source("pkt A { h: 8 = 0xAA;\n"
                           "  [ @byte_order: little_endian; ] a: 32;\n"
                           "};",
                           &written);

  int failure = 0;
  if (!ok) {
    failure = __LINE__;
  } else {
    const FieldInstruction *from_block = &inherited.program->packets[0].fields[1];
    const FieldInstruction *from_field = &written.program->packets[0].fields[1];
    if (from_block->byte_order != BYTE_ORDER_LITTLE_ENDIAN ||
        from_block->byte_order != from_field->byte_order) {
      failure = __LINE__;
    } else if (from_block->bit_order != from_field->bit_order) {
      failure = __LINE__;
    }
  }

  compiled_free(&inherited);
  compiled_free(&written);
  return failure;
}

/* Every packet in the file, not just the first, and the fields of one do not run into the next. */
static int us8_tc11_several_packets_compile_independently(const TestArguments *args) {
  (void)args;

  Compiled compiled;
  if (!compile_source("pkt A { t: 8 = 1; a: 8; };\npkt B { t: 8 = 2; b: 16; c: 8; };", &compiled)) {
    compiled_free(&compiled);
    FORCE_FAILURE();
  }

  int failure = 0;
  if (compiled.program->packet_count != 2) {
    failure = __LINE__;
  } else if (compiled.program->packets[0].field_count != 2 ||
             compiled.program->packets[1].field_count != 3) {
    failure = __LINE__;
  } else if (compiled.program->packets[1].fields[0].fixed != 2) {
    failure = __LINE__;
  } else if (compiled.program->packets[1].fields[1].width_bits != 16) {
    failure = __LINE__;
  }

  compiled_free(&compiled);
  return failure;
}

static int us8_tc12_every_example_compiles(const TestArguments *args) {
  (void)args;

  /* The same shapes the example files hold, kept here as sources so this story needs no file
   * system -- examples/ is walked by the system tests, which drive the real binary over the real
   * files. */
  const char *sources[] = {
      "@bit_order: msb_first;\npkt A { h: 8 = 0xAA; n: 32; d[128]: valof(n) * 8; c: 16; };",
      "pkt A { v: 3 = 0b000; t: 1 = 0b0; f: 1; i: 11; s: 2; q: 14; l: 16;"
      " u[65536]: (valof(l) + 1) * 8; };",
      "pkt A { t: 8 = 1; s: 16; };\npkt B { t: 8 = 2; s: 16; m: 8; };",
  };

  int failure = 0;
  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    Compiled compiled;
    bool ok = compile_source(sources[i], &compiled);
    compiled_free(&compiled);
    if (!ok) {
      failure = __LINE__;
      break;
    }
  }
  return failure;
}

void register_user_story8(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us8_tc01_a_constant_packet_compiles_field_by_field,
                "one instruction per field, in declaration order", false);
  ADD_TEST_CASE(us8_tc02_a_constant_width_compiles_to_no_program,
                "a constant width costs no evaluation", false);
  ADD_TEST_CASE(us8_tc03_a_valof_width_compiles_to_postfix,
                "valof(n) * 8 compiles to SLOT LIT MUL", false);
  ADD_TEST_CASE(us8_tc04_constant_subtrees_are_folded, "a constant subtree folds to one literal",
                false);
  ADD_TEST_CASE(us8_tc05_a_nested_expression_evaluates_correctly,
                "(valof(n) + 1) * 8 evaluates in the right order", false);
  ADD_TEST_CASE(us8_tc06_every_operator_evaluates, "+ - * / all evaluate", false);
  ADD_TEST_CASE(us8_tc07_the_stack_depth_is_the_true_maximum,
                "the computed stack depth is the real maximum", false);
  ADD_TEST_CASE(us8_tc08_a_runaway_width_is_refused,
                "an overflowing or negative width is refused per frame", false);
  ADD_TEST_CASE(us8_tc09_a_fixed_field_carries_its_value,
                "a fixed value travels on the instruction", false);
  ADD_TEST_CASE(us8_tc10_orders_are_carried_resolved,
                "inherited and written orders compile identically", false);
  ADD_TEST_CASE(us8_tc11_several_packets_compile_independently,
                "each packet gets its own instructions", false);
  ADD_TEST_CASE(us8_tc12_every_example_compiles, "the example shapes all compile", false);
}
