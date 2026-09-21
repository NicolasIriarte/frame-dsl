#pragma once

/**
 * @file
 * @brief Calibrations, compiled: the functions `cal` declares, in the form the runtime runs them.
 *
 * Internal, like the rest of the bytecode. A calibration becomes one of two things:
 *
 *   - **Linear**, when its body reduces to `scale * x + offset` -- which most real curves do, and
 *     which is then two floating-point operations with no interpreter involved.
 *   - **Code** otherwise: a short postfix program over a fixed stack of doubles, the same shape as a
 *     width program. Calls to other calibrations are inlined when the file loads, with the argument
 *     held in a register, so a program never calls anything.
 *
 * Constant subtrees are folded first, in both cases.
 *
 * The C that --emit-c writes is generated from this compiled form rather than from the source, so it
 * computes what the interpreter computes, operation for operation.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ast.h"
#include "diag.h"

/** @brief Deep enough for any curve anyone writes; shallow enough to be a fixed array. */
#define CAL_STACK_MAX 32

/** @brief How deeply calls may nest, since each level holds its argument in a register. */
#define CAL_REGISTER_MAX 16

/** @brief The most instructions one calibration may inline to, so that a file cannot make loading
 * exponential by calling a curve twice from a curve called twice from a curve... */
#define CAL_CODE_MAX 4096

typedef enum {
  /** @brief Push register N. Register 0 is the function's own argument. */
  CAL_OP_LOAD,
  /** @brief Pop into register N: the argument of an inlined call. */
  CAL_OP_STORE,
  CAL_OP_LIT,
  CAL_OP_ADD,
  CAL_OP_SUB,
  CAL_OP_MUL,
  CAL_OP_DIV,
  CAL_OP_NEG,
} CalOp;

typedef struct {
  CalOp op;
  uint32_t reg;
  double literal;
} CalInstruction;

/** @brief One direction of a calibration. */
typedef struct {
  bool linear;
  double scale;
  double offset;

  /** @brief When not linear. */
  const CalInstruction *code;
  uint32_t length;
} CalFunction;

typedef struct {
  const char *name;
  CalFunction forward;

  /** @brief Whether an engineering value can be turned back into a raw one at all. */
  bool invertible;
  /**
   * @brief True when the inverse is the forward line solved for x, computed as
   * `(y - offset) / scale` -- which is not the same double as `y * (1 / scale) - offset / scale`,
   * and is why it is not simply stored as another line.
   */
  bool inverse_solves_line;
  /** @brief The `inverse` clause, when one was written. */
  CalFunction inverse;
} CalCompiled;

typedef struct {
  const CalCompiled *cals;
  size_t count;
} CalTable;

/**
 * @brief Compiles every calibration in a checked file. One allocation, freed by cal_table_free().
 * @param file A file that passed check_file().
 * @param diags Reported into on failure.
 * @return The table -- empty, not NULL, when the file declares none -- or NULL with a report on
 * failure, which after a clean check means an allocation failed or a curve is too large.
 */
CalTable *cal_compile(const AstFile *file, DiagList *diags);

void cal_table_free(CalTable *table);

/**
 * @brief Runs one direction of a calibration.
 * @return false when the result is not a finite number -- a division by a value that happened to
 * be zero, or an overflow.
 */
bool cal_eval(const CalFunction *function, double x, double *out);

/** @brief The forward curve of `cal`, applied to `raw`. */
bool cal_forward(const CalCompiled *cal, double raw, double *out);

/** @brief The inverse of `cal`, applied to `eng`. false as well when it has none. */
bool cal_backward(const CalCompiled *cal, double eng, double *out);

/**
 * @brief Writes a C header of `static inline double` functions, one per calibration and one per
 * inverse, snprintf-style.
 * @param table The compiled calibrations.
 * @param prefix Put in front of every function name.
 * @param out Written up to `capacity`, always terminated when `capacity` is not zero.
 * @param capacity Size of `out`.
 * @return The length of the whole header, not counting the terminator, whatever `capacity` was.
 */
size_t cal_emit_c(const CalTable *table, const char *prefix, char *out, size_t capacity);
