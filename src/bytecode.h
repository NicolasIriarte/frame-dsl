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

  /** @brief Resolved: what applies after file, packet, block and field scope are combined. */
  BitOrder bit_order;
  ByteOrder byte_order;
} FieldInstruction;

typedef struct {
  const FieldInstruction *fields;
  size_t field_count;
} PacketProgram;

typedef struct {
  const PacketProgram *packets;
  size_t packet_count;
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
