#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "region.h"

typedef enum {
  BIT_ORDER_MSB_FIRST,
  BIT_ORDER_LSB_FIRST,
} BitOrder;

typedef enum {
  BYTE_ORDER_BIG_ENDIAN,
  BYTE_ORDER_LITTLE_ENDIAN,
} ByteOrder;

typedef enum {
  EXPR_LITERAL,
  EXPR_VALOF,
  EXPR_BINARY,
} ExprKind;

typedef struct Expr {
  ExprKind kind;
  int line;
  int column;

  uint64_t literal; /**< EXPR_LITERAL */
  const char *reference; /**< EXPR_VALOF */

  char op; /**< EXPR_BINARY: one of + - * / */
  struct Expr *lhs;
  struct Expr *rhs;
} Expr;

/* ---- Calibrations ---------------------------------------------------------------------------
 *
 * A calibration's arithmetic is in doubles and has a parameter and calls, none of which a width has.
 * It is a separate node type rather than more kinds on Expr so that nothing in the width path -- the
 * checker's folds, the byte-multiple analysis, the width compiler -- has a case it must ignore. */

typedef enum {
  CAL_EXPR_NUMBER,
  /** @brief A bare name: the parameter, if it is spelled like it. Check 19 says if it is not. */
  CAL_EXPR_NAME,
  /** @brief `name(argument)`: another calibration, applied to `lhs`. */
  CAL_EXPR_CALL,
  CAL_EXPR_NEGATE,
  CAL_EXPR_BINARY,
} CalExprKind;

typedef struct CalExpr {
  CalExprKind kind;
  int line;
  int column;

  double number; /**< CAL_EXPR_NUMBER */
  const char *name; /**< CAL_EXPR_NAME, CAL_EXPR_CALL */

  char op; /**< CAL_EXPR_BINARY: one of + - * / */
  struct CalExpr *lhs; /**< the operand, the argument, or the left-hand side */
  struct CalExpr *rhs;
} CalExpr;

/** @brief `cal name(param) = body [inverse (inverse_param) = inverse];` */
typedef struct {
  const char *name;
  int line;
  int column;

  const char *param;
  CalExpr *body;

  /** @brief NULL when none was written, which is the usual case: a linear curve inverts itself. */
  const char *inverse_param;
  CalExpr *inverse;
  int inverse_line;
  int inverse_column;
} AstCal;

typedef struct {
  const char *name;
  int line;
  int column;

  Expr *width;

  /**
   * @brief The arena reservation written as `name[bytes]:`, or NULL when there is none.
   *
   * In BYTES, unlike every other number in the language, because it is storage rather than layout --
   * see manual/language.md. check_file() converts it to bits once and nothing downstream sees bytes.
   */
  Expr *reserve;
  int reserve_line;
  int reserve_column;

  bool has_fixed;
  uint64_t fixed;
  int fixed_line;
  int fixed_column;

  /** @brief Resolved: what applies to this field once file, packet and field scope are combined. */
  BitOrder bit_order;
  ByteOrder byte_order;

  /**
   * @brief Whether the setting was *written on this field*, as opposed to inherited.
   *
   * Check 10 turns on the difference: an inherited byte_order that cannot apply here is irrelevant,
   * while one written here means somebody believed they had specified something.
   */
  bool bit_order_written;
  bool byte_order_written;
  int bit_order_line;
  int bit_order_column;
  int byte_order_line;
  int byte_order_column;

  /** @brief The calibration `@cal:` names, or NULL. Resolved by name at check time (check 18). */
  const char *cal;
  int cal_line;
  int cal_column;
} AstField;

typedef struct {
  const char *name;
  int line;
  int column;

  AstField *fields;
  size_t field_count;
  size_t field_capacity;
} AstPacket;

typedef struct {
  AstPacket *packets;
  size_t packet_count;
  size_t packet_capacity;

  AstCal *cals;
  size_t cal_count;
  size_t cal_capacity;

  /** @brief Owns every Expr and every string the nodes point at. */
  Region *region;
} AstFile;

AstFile *ast_new(void);
void ast_free(AstFile *file);

/**
 * @brief Add a packet to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstPacket *ast_add_packet(AstFile *file);

/**
 * @brief Add a field to `packet`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstField *ast_add_field(AstPacket *packet);

/**
 * @brief Add a calibration to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstCal *ast_add_cal(AstFile *file);
