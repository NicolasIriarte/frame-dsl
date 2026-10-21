#pragma once

/**
 * @file
 * @brief The checked model, compiled to something a loop can walk.
 *
 * Internal, and deliberately so: this is not a file format, is not stable, and no user ever sees it.
 * The definition file is the only input to the library.
 *
 * Requirement 3 in docs/scope.md is that the intermediate form be a bytecode meant to be interpreted
 * quickly rather than a tree walked with pointer chasing. That is what this is. A packet becomes a
 * flat array of field instructions in declaration order, each carrying everything the encoder or the
 * decoder needs for that field and nothing else -- no node type to branch on, no pointer to chase,
 * and the next field is the next array element.
 *
 * Widths are the one thing not known ahead of time, and only for a valof()-sized field. Those compile
 * to a short postfix program over a small operand stack, whose only non-literal operand is "the value
 * in slot N". Check 7 guarantees N was decoded before this field is reached.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "check.h"
#include "diag.h"

/**
 * @brief Deep enough for any width expression anyone would write -- `valof(len) * 8 - 32` reaches
 * two -- and shallow enough that the evaluator's stack is a fixed array rather than an allocation.
 *
 * A program needing more is rejected when the definition loads.
 */
#define WIDTH_STACK_MAX 32

typedef enum {
  /** @brief Push the value of the packet's field N. */
  OP_SLOT,
  /** @brief Push a literal. */
  OP_LIT,
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
} WidthOp;

typedef struct {
  WidthOp op;
  uint64_t operand;
} WidthInstruction;

/**
 * @brief A width that depends on a frame.
 *
 * `code` is NULL when the width is constant, which is the common case and costs the interpreter
 * nothing: it reads `width_bits` off the field instruction and never touches the evaluator.
 */
typedef struct {
  const WidthInstruction *code;
  uint32_t length;
  /** @brief The deepest the operand stack gets, computed here so the evaluator never checks. */
  uint32_t stack_depth;
} WidthProgram;

/** @brief Where a condition's walk ends: past every instruction, decided (#126). */
#define CONDITION_TRUE UINT32_MAX
#define CONDITION_FALSE (UINT32_MAX - 1)

/**
 * @brief One comparison a field's presence depends on (#12, #126): the value in slot `field`, ANDed with
 * `mask`, compared with `value`. A field's comparisons are a little graph rather than a list: each says
 * where to go next when it holds and when it does not -- another comparison of the same field's, or
 * CONDITION_TRUE or CONDITION_FALSE -- which is `&&` and `||` short-circuited, with no stack to keep.
 */
typedef struct {
  uint32_t field;
  CompareOp op;
  /** @brief All ones when the condition writes no mask. */
  uint64_t mask;
  uint64_t value;
  uint32_t if_true;
  uint32_t if_false;
} ConditionInstruction;

/**
 * @brief One field of a repeated block's element (#55): where it sits inside an element, and how to
 * read it. Every width is constant -- composition refuses to repeat a block whose size varies.
 */
typedef struct {
  const char *name;
  uint64_t offset_bits;
  uint64_t width_bits;
  bool is_scalar;
  bool has_fixed;
  uint64_t fixed;
  int32_t cal;
  /** @brief As FieldInstruction.enumeration. */
  int32_t enumeration;
  BitOrder bit_order;
  ByteOrder byte_order;
} MemberInstruction;

/** @brief A repeated block's element, laid out once however many repetitions use it. */
typedef struct {
  const struct AstPacket *block;
  MemberInstruction *members;
  size_t member_count;
  uint64_t bits;
} ElementProgram;

/**
 * @brief One field. Everything the runtime needs for it, flat.
 *
 * There is no arena offset here. A program describes a definition, not a frame, and several frames
 * over one definition each own their own arena -- so the instruction carries the field's index and
 * the frame maps that to storage.
 */
typedef struct {
  uint32_t field;

  bool variable;
  /** @brief The width, when it is constant. Zero when it is not. */
  uint64_t width_bits;
  /** @brief Evaluated per frame, when it is not. */
  WidthProgram width;
  /** @brief What the field reserved, which is also the most a frame may claim for it. */
  uint64_t max_bits;

  bool is_scalar;

  /**
   * @brief Written from here rather than from the arena, so a caller cannot emit a frame that fails
   * to identify itself.
   */
  bool has_fixed;
  uint64_t fixed;

  /** @brief Index into the definition's calibrations, or -1. Resolved here so that reading an
   * engineering value never searches by name. */
  int32_t cal;

  /** @brief Index into the definition's enums, or -1: what names this field's values (#11). */
  int32_t enumeration;

  /** @brief Resolved: what applies after file, packet, block and field scope are combined. */
  BitOrder bit_order;
  ByteOrder byte_order;

  /**
   * @brief For a repetition (#55), what one element is; NULL for any other field. The field itself
   * is walked as a bit string of `count * element->bits`; this is only for reading inside it, and for
   * the fixed values its elements carry, which encode writes into every element.
   */
  const ElementProgram *element;

  /**
   * @brief What the field's presence depends on (#12): its whole condition, every term of every `@if`
   * around it, walked from `condition_entry` over values already read -- a condition only names an
   * earlier field. Zero comparisons is the common case, and costs the walk one comparison.
   */
  const ConditionInstruction *conditions;
  uint32_t condition_count;
  uint32_t condition_entry;
} FieldInstruction;

typedef struct {
  const FieldInstruction *fields;
  size_t field_count;
} PacketProgram;

typedef struct {
  const PacketProgram *packets;
  size_t packet_count;
  /** @brief Every repeated block's element, each once. Allocated apart from the rest. */
  ElementProgram *elements;
  size_t element_count;
} Program;

/**
 * @brief Compiles a checked file.
 *
 * One allocation for the whole program, freed by program_free().
 *
 * @param file The checked AST.
 * @param layout The layout check_file() produced for it.
 * @param diags Reported into on failure.
 * @return The compiled program, or NULL and a report into `diags` on failure, which after a clean
 * check can only be an allocation failure or an expression nested past WIDTH_STACK_MAX.
 */
Program *program_compile(const AstFile *file, const Layout *layout, DiagList *diags);

void program_free(Program *program);

/**
 * @brief Runs a width program.
 * @param program The program to run.
 * @param slots Holds the packet's field values, indexed as OP_SLOT indexes them, and must have at
 * least as many entries as the packet has fields.
 * @param out Set to the result on success.
 * @return false on a division by zero or a width past what any frame could hold -- both of which
 * depend on a frame's contents, so neither can be settled when the definition loads.
 */
bool width_program_eval(const WidthProgram *program, const uint64_t *slots, uint64_t *out);

/** @brief Whether one comparison holds for `value`, the field it reads. */
static inline bool condition_test(const ConditionInstruction *condition, uint64_t value) {
  value &= condition->mask;
  switch (condition->op) {
  case COMPARE_EQ:
    return value == condition->value;
  case COMPARE_NE:
    return value != condition->value;
  case COMPARE_LT:
    return value < condition->value;
  case COMPARE_LE:
    return value <= condition->value;
  case COMPARE_GT:
    return value > condition->value;
  case COMPARE_GE:
    return value >= condition->value;
  }
  return false;
}

/**
 * @brief Whether a field is present (#12): its condition holds over `slots`, indexed as OP_SLOT indexes
 * them. A field with no conditions always is.
 */
static inline bool field_present(const FieldInstruction *instruction, const uint64_t *slots) {
  uint32_t at = instruction->condition_entry;
  while (at < instruction->condition_count) {
    const ConditionInstruction *condition = &instruction->conditions[at];
    at = condition_test(condition, slots[condition->field]) ? condition->if_true : condition->if_false;
  }
  return at != CONDITION_FALSE;
}
