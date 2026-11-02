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
#include "bits.h"
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
  /** @brief A signed field's value (#10): the slot in the operand's low half, its width in the high. */
  OP_SLOT_SIGNED,
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
  /** @brief It reads a signed field, so a term may go below zero on the way to the result (#10). */
  bool has_signed;
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
  /** @brief The width of a signed field compared as signed, or 0 (#10). `value` is then its 64-bit
   * two's complement. */
  uint8_t sign_bits;
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
  /** @brief As FieldInstruction.has_default. */
  bool has_default;
  uint64_t default_value;
  FieldKind kind;
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
  /**
   * @brief The element's fixed values as bytes (#101): of the element's bytes, the `fixed_count` that
   * hold a fixed bit, at `fixed_at`, with which of their bits are fixed in `fixed_mask` and what to in
   * `fixed_value`. Encode stamps them into every element of a repetition on a byte boundary, rather
   * than writing each fixed member of each element in turn. One allocation, at `fixed_at`.
   */
  size_t fixed_count;
  /** @brief How many members are fixed: none is common, and leaves encode nothing to stamp. */
  size_t fixed_members;
  /** @brief How many members have a default, which only a frame starting out has to write (#144). */
  size_t default_members;
  uint32_t *fixed_at;
  uint8_t *fixed_mask;
  uint8_t *fixed_value;
} ElementProgram;

/** @brief Whether a member is `little_endian` as whole bytes on a byte boundary within its element, which
 * is those bytes swapped. */
static inline bool member_swapped(const MemberInstruction *member) {
  return member->byte_order == BYTE_ORDER_LITTLE_ENDIAN && member->width_bits % 8 == 0 && member->offset_bits % 8 == 0;
}

/** @brief Whether a member is `little_endian` and anything else, filling bytes from the low bit (#147). */
static inline bool member_packed(const MemberInstruction *member) {
  return member->byte_order == BYTE_ORDER_LITTLE_ENDIAN && !member_swapped(member) && member->width_bits > 0;
}

/**
 * @brief A member's value out of wire bytes at bit `offset`, in the order encode.c lays a field down and
 * decode.c reads it back: reversed for lsb_first, swapped for little_endian (#101).
 */
static inline uint64_t member_get(const uint8_t *bytes, uint64_t offset, const MemberInstruction *member) {
  uint64_t value = member_packed(member) ? bits_get_le(bytes, offset, member->width_bits)
                                         : bits_get(bytes, offset, member->width_bits);
  if (member->bit_order == BIT_ORDER_LSB_FIRST && member->width_bits > 0) {
    value = bits_reverse(value, member->width_bits);
  }
  return member_swapped(member) ? bits_swap(value, member->width_bits) : value;
}

/** @brief Writes a member into wire bytes at bit `offset`, in its own orders, clearing what was there. */
static inline void member_put(uint8_t *bytes, uint64_t offset, const MemberInstruction *member, uint64_t value) {
  if (member_swapped(member)) {
    value = bits_swap(value, member->width_bits);
  }
  if (member->bit_order == BIT_ORDER_LSB_FIRST && member->width_bits > 0) {
    value = bits_reverse(value, member->width_bits);
  }
  if (member_packed(member)) {
    bits_put_le(bytes, offset, member->width_bits, value);
    return;
  }
  bits_put(bytes, offset, member->width_bits, value);
}

/**
 * @brief A repetition's fixed values, into every element of the `width` bits at `offset` (#101).
 *
 * On a byte boundary, which is the common case, an element's fixed bits are a few bytes masked in; off
 * one, each fixed member is written in turn.
 */
static inline void stamp_fixed(uint8_t *out, uint64_t offset, uint64_t width, const ElementProgram *element) {
  if (element->bits == 0 || element->fixed_members == 0) {
    return;
  }
  if (offset % 8 == 0 && element->fixed_at != NULL) {
    const size_t step = (size_t)(element->bits / 8);
    uint8_t *at = out + offset / 8;
    for (uint64_t start = 0; start < width; start += element->bits, at += step) {
      for (size_t i = 0; i < element->fixed_count; ++i) {
        uint8_t *byte = &at[element->fixed_at[i]];
        *byte = (uint8_t)((*byte & ~element->fixed_mask[i]) | element->fixed_value[i]);
      }
    }
    return;
  }
  for (uint64_t start = 0; start < width; start += element->bits) {
    for (size_t m = 0; m < element->member_count; ++m) {
      const MemberInstruction *member = &element->members[m];
      if (member->has_fixed) {
        member_put(out, offset + start + member->offset_bits, member, member->fixed);
      }
    }
  }
}

/**
 * @brief How a field of a packet's known prefix moves (#106), worked out once at compile time.
 *
 * The walk tests these in this order, most common first, with an `if` per kind rather than a `switch`:
 * with a kind per combination of orders the switch compiled to a jump table, an indirect branch per
 * field, and decoding slowed by up to a third across the board.
 */
typedef enum {
  /** @brief A scalar of whole bytes on a byte boundary, `msb_first` and big-endian: its bytes as they are. */
  COPY_BYTES,
  /** @brief Anything else: write_scalar() / read_scalar(), which handle every shape. */
  COPY_GENERAL,
  /** @brief Whole bytes on a byte boundary, `little_endian`: its bytes the other way round. */
  COPY_BYTES_SWAPPED,
  /** @brief A fixed-width bit string: copied, or shifted off a boundary. */
  COPY_STRING,
  /** @brief Whole bytes on a byte boundary, `lsb_first` (#108): each byte bit-reversed by table, after
   * a byte swap when the instruction's `copy_swap` says it is `little_endian` too. */
  COPY_BYTES_REVERSED,
} CopyKind;

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

  /** @brief What the slot starts with when nothing has set it (#144). Only a frame starting out reads
   * this: from then on the value is in the arena, like any other. */
  bool has_default;
  uint64_t default_value;

  /** @brief How the slot's pattern reads (#10). Nothing on the wire depends on it. */
  FieldKind kind;

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
  /** @brief For a repetition, the most elements it has room for: what it reserves, in whole elements.
   * Kept here so that reaching an element does not work it out again on every call (#101). */
  uint64_t element_capacity;

  /**
   * @brief What the field's presence depends on (#12): its whole condition, every term of every `@if`
   * around it, walked from `condition_entry` over values already read -- a condition only names an
   * earlier field. Zero comparisons is the common case, and costs the walk one comparison.
   */
  const ConditionInstruction *conditions;
  uint32_t condition_count;
  uint32_t condition_entry;

  /** @brief For a field of the packet's known prefix (#106): where it starts, and how it moves. */
  uint64_t offset_bits;
  CopyKind copy;
  /** @brief For COPY_BYTES_REVERSED: `little_endian` as well. */
  bool copy_swap;
} FieldInstruction;

typedef struct {
  const FieldInstruction *fields;
  size_t field_count;
  /**
   * @brief The packet's known prefix (#106): its first `prefix_count` fields sit at a constant offset,
   * have a constant width and no condition, and are no repetition, and end at bit `prefix_bits`. For
   * most packets that is every field. The walk moves them without asking any of those questions, and a
   * decode checks the frame's length against the prefix once instead of field by field.
   */
  size_t prefix_count;
  uint64_t prefix_bits;
  /** @brief `@decode: manual`: only framedsl_decode_as() decodes it (#146). */
  bool manual;
  /** @brief The most bytes a frame of this packet can take, every field at its reservation: what an
   * encode clears before it starts. */
  size_t max_bytes;
  /** @brief Whether anything reads another field's value as the walk goes: a `valof()` width or a
   * condition. Without one, encode does not gather the values first. */
  bool reads_values;
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
  /* Two's complement orders as unsigned does with the sign bit flipped, so the switch below serves. */
  uint64_t constant = condition->value;
  if (condition->sign_bits != 0) {
    const uint64_t flip = (uint64_t)1 << 63;
    value = (uint64_t)bits_sign_extend(value, condition->sign_bits) ^ flip;
    constant ^= flip;
  }
  switch (condition->op) {
  case COMPARE_EQ:
    return value == constant;
  case COMPARE_NE:
    return value != constant;
  case COMPARE_LT:
    return value < constant;
  case COMPARE_LE:
    return value <= constant;
  case COMPARE_GT:
    return value > constant;
  case COMPARE_GE:
    return value >= constant;
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
