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

  uint64_t literal; /* EXPR_LITERAL */
  const char *reference; /* EXPR_VALOF */

  char op; /* EXPR_BINARY: one of + - * / */
  struct Expr *lhs;
  struct Expr *rhs;
} Expr;

typedef struct {
  const char *name;
  int line;
  int column;

  Expr *width;

  /* The arena reservation written as `name[bytes]:`, or NULL when there is none. In BYTES, unlike
   * every other number in the language, because it is storage rather than layout -- see
   * manual/language.md. check_file() converts it to bits once and nothing downstream sees bytes. */
  Expr *reserve;
  int reserve_line;
  int reserve_column;

  bool has_fixed;
  uint64_t fixed;
  int fixed_line;
  int fixed_column;

  /* Resolved: what applies to this field once file, packet and field scope are combined. */
  BitOrder bit_order;
  ByteOrder byte_order;

  /* Whether the setting was *written on this field*, as opposed to inherited. Check 10 turns on the
   * difference: an inherited byte_order that cannot apply here is irrelevant, while one written here
   * means somebody believed they had specified something. */
  bool bit_order_written;
  bool byte_order_written;
  int bit_order_line;
  int bit_order_column;
  int byte_order_line;
  int byte_order_column;
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

  /* Owns every Expr and every string the nodes point at. */
  Region *region;
} AstFile;

AstFile *ast_new(void);
void ast_free(AstFile *file);

/* Return NULL on allocation failure. The caller reports it; there is nothing else to do. */
AstPacket *ast_add_packet(AstFile *file);
AstField *ast_add_field(AstPacket *packet);
