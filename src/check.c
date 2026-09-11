#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A width no sane frame reaches, used to stop a fold from running away rather than to express a real
 * limit. 2^32 bits is half a gigabyte in one field. */
#define WIDTH_CEILING ((int64_t)1 << 32)

typedef struct {
  const AstFile *file;
  DiagList *diags;
  Layout *layout;
  bool failed;
} Checker;

static void fail(Checker *checker, int line, int column, int check, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));

static void fail(Checker *checker, int line, int column, int check, const char *fmt, ...) {
  checker->failed = true;

  va_list args;
  va_start(args, fmt);
  char message[512];
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  diag_add(checker->diags, line, column, check, "%s", message);
}

/* ---- Constant folding ----------------------------------------------------------------------- */

typedef enum {
  FOLD_OK,
  FOLD_NOT_CONSTANT, /* contains valof() */
  FOLD_DIVIDE_BY_ZERO,
  FOLD_NEGATIVE,
  FOLD_TOO_LARGE,
} FoldResult;

/* Evaluated signed, because subtraction is how a length that counts itself is expressed
 * (`valof(len) * 8 - 32`) and an intermediate step may legitimately be smaller than what came
 * before. A negative *result* is still an error -- nothing has a negative width. */
static FoldResult fold(const Expr *expr, int64_t *out) {
  if (expr == NULL) {
    return FOLD_NOT_CONSTANT;
  }

  switch (expr->kind) {
  case EXPR_LITERAL:
    if (expr->literal > (uint64_t)WIDTH_CEILING) {
      return FOLD_TOO_LARGE;
    }
    *out = (int64_t)expr->literal;
    return FOLD_OK;

  case EXPR_VALOF:
    return FOLD_NOT_CONSTANT;

  case EXPR_BINARY: {
    int64_t lhs = 0;
    int64_t rhs = 0;
    FoldResult result = fold(expr->lhs, &lhs);
    if (result != FOLD_OK) {
      return result;
    }
    result = fold(expr->rhs, &rhs);
    if (result != FOLD_OK) {
      return result;
    }

    switch (expr->op) {
    case '+': *out = lhs + rhs; break;
    case '-': *out = lhs - rhs; break;
    case '*':
      if (lhs != 0 && (rhs > WIDTH_CEILING / lhs || rhs < -WIDTH_CEILING / lhs)) {
        return FOLD_TOO_LARGE;
      }
      *out = lhs * rhs;
      break;
    case '/':
      if (rhs == 0) {
        return FOLD_DIVIDE_BY_ZERO;
      }
      *out = lhs / rhs;
      break;
    default: return FOLD_NOT_CONSTANT;
    }

    if (*out > WIDTH_CEILING) {
      return FOLD_TOO_LARGE;
    }
    return FOLD_OK;
  }
  }
  return FOLD_NOT_CONSTANT;
}

static bool expr_uses_valof(const Expr *expr) {
  if (expr == NULL) {
    return false;
  }
  if (expr->kind == EXPR_VALOF) {
    return true;
  }
  if (expr->kind == EXPR_BINARY) {
    return expr_uses_valof(expr->lhs) || expr_uses_valof(expr->rhs);
  }
  return false;
}

/* Check 7: a valof() may only name a field declared earlier in the same packet. Reading a field
 * means knowing where it starts, which means every field before it is already resolved. */
static void check_references(Checker *checker, const AstPacket *packet, size_t field_index,
                             const Expr *expr) {
  if (expr == NULL) {
    return;
  }
  if (expr->kind == EXPR_BINARY) {
    check_references(checker, packet, field_index, expr->lhs);
    check_references(checker, packet, field_index, expr->rhs);
    return;
  }
  if (expr->kind != EXPR_VALOF) {
    return;
  }

  for (size_t i = 0; i < field_index; ++i) {
    if (strcmp(packet->fields[i].name, expr->reference) == 0) {
      return;
    }
  }

  /* Distinguish "later in this packet" from "nowhere here": they are different mistakes, and the
   * second is usually a field belonging to another packet. */
  for (size_t i = field_index; i < packet->field_count; ++i) {
    if (strcmp(packet->fields[i].name, expr->reference) == 0) {
      fail(checker, expr->line, expr->column, 7,
           "valof(%s) names a field declared later in '%s'; a width can only depend on a field "
           "already read",
           expr->reference, packet->name);
      return;
    }
  }

  fail(checker, expr->line, expr->column, 7, "valof(%s) names no field in '%s'", expr->reference,
       packet->name);
}

/* ---- Fixed-bit projection (check 12) -------------------------------------------------------- */

static void bits_set(uint8_t *bits, size_t index) {
  bits[index / 8] |= (uint8_t)(1u << (index % 8));
}

/* Lays a fixed value onto the stream bits its field occupies, applying that field's bit order and
 * byte order first -- the question is which *stream* bits are pinned, not which bits of the value
 * are. */
static void project(const AstField *field, uint64_t offset, uint64_t width, uint8_t *mask,
                    uint8_t *value) {
  uint64_t wire = field->fixed;

  if (field->byte_order == BYTE_ORDER_LITTLE_ENDIAN && width % 8 == 0 && offset % 8 == 0) {
    uint64_t swapped = 0;
    for (uint64_t byte = 0; byte < width / 8; ++byte) {
      swapped = (swapped << 8) | ((wire >> (byte * 8)) & 0xFF);
    }
    wire = swapped;
  }

  for (uint64_t j = 0; j < width; ++j) {
    uint64_t bit = field->bit_order == BIT_ORDER_MSB_FIRST ? (wire >> (width - 1 - j)) & 1
                                                           : (wire >> j) & 1;
    bits_set(mask, (size_t)(offset + j));
    if (bit != 0) {
      bits_set(value, (size_t)(offset + j));
    }
  }
}

/* ---- Per-packet checks ---------------------------------------------------------------------- */

static void check_packet(Checker *checker, size_t packet_index) {
  const AstPacket *packet = &checker->file->packets[packet_index];
  PacketLayout *layout = &checker->layout->packets[packet_index];

  layout->fields = calloc(packet->field_count == 0 ? 1 : packet->field_count, sizeof(FieldLayout));
  if (layout->fields == NULL) {
    fail(checker, packet->line, packet->column, 0, "out of memory");
    return;
  }
  layout->field_count = packet->field_count;

  uint64_t offset = 0;
  bool offset_known = true;
  uint64_t discriminator_bits = 0;
  bool has_fixed = false;

  /* First pass: widths, offsets, and everything decidable field by field. */
  for (size_t i = 0; i < packet->field_count; ++i) {
    const AstField *field = &packet->fields[i];
    FieldLayout *slot = &layout->fields[i];

    /* Check 2. */
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(packet->fields[j].name, field->name) == 0) {
        fail(checker, field->line, field->column, 2, "'%s' already has a field called '%s'",
             packet->name, field->name);
        break;
      }
    }

    check_references(checker, packet, i, field->width);
    check_references(checker, packet, i, field->max);

    bool variable = expr_uses_valof(field->width);
    slot->variable = variable;

    if (variable) {
      /* Check 5. */
      if (field->max == NULL) {
        fail(checker, field->line, field->column, 5,
             "'%s.%s' has a width that depends on another field, so it must declare a bound with "
             "'max'; without one it cannot be given a fixed slot in the arena",
             packet->name, field->name);
      }
    } else {
      /* Check 4. */
      int64_t width = 0;
      FoldResult folded = fold(field->width, &width);
      if (folded == FOLD_DIVIDE_BY_ZERO) {
        fail(checker, field->line, field->column, 4, "'%s.%s' has a width that divides by zero",
             packet->name, field->name);
      } else if (folded == FOLD_TOO_LARGE) {
        fail(checker, field->line, field->column, 4, "'%s.%s' has an impossibly large width",
             packet->name, field->name);
      } else if (folded != FOLD_OK || width <= 0) {
        fail(checker, field->line, field->column, 4,
             "'%s.%s' must have a width of at least one bit", packet->name, field->name);
      } else {
        slot->width_bits = (uint64_t)width;
      }
    }

    /* Check 6. */
    if (field->max != NULL) {
      int64_t bound = 0;
      FoldResult folded = fold(field->max, &bound);
      if (folded == FOLD_NOT_CONSTANT) {
        fail(checker, field->max->line, field->max->column, 6,
             "'%s.%s' has a 'max' that depends on another field; a bound must be a constant, since "
             "it is what the arena reserves before any frame arrives",
             packet->name, field->name);
      } else if (folded != FOLD_OK || bound <= 0) {
        fail(checker, field->max->line, field->max->column, 6,
             "'%s.%s' must have a 'max' of at least one bit", packet->name, field->name);
      } else {
        slot->max_bits = (uint64_t)bound;
      }
    }

    /* Check 9. */
    if (field->has_fixed && field->max != NULL) {
      fail(checker, field->fixed_line, field->fixed_column, 9,
           "'%s.%s' cannot have both a fixed value and 'max': a width that is not known until a "
           "frame arrives cannot identify the packet that frame belongs to",
           packet->name, field->name);
    }

    slot->reserved_bits = variable ? slot->max_bits : slot->width_bits;
    slot->is_scalar = !variable && slot->width_bits > 0 && slot->width_bits <= FRAMEDSL_SCALAR_MAX_BITS;
    slot->offset_known = offset_known;
    slot->offset_bits = offset_known ? offset : 0;

    /* Check 8. */
    if (field->has_fixed) {
      has_fixed = true;
      uint64_t width = slot->width_bits;
      if (!variable && width > 0 && width < 64 && field->fixed >= ((uint64_t)1 << width)) {
        fail(checker, field->fixed_line, field->fixed_column, 8,
             "'%s.%s' is %llu bits, which cannot hold the value %#llx", packet->name, field->name,
             (unsigned long long)width, (unsigned long long)field->fixed);
      }
    }

    /* Check 10. Only a byte_order *written here* is an error: one inherited from packet or file
     * scope covers every field below it, and most real frames mix byte-wide fields with sub-byte
     * ones. What is worth rejecting is somebody writing a setting that cannot do anything. */
    if (field->byte_order_written && !variable && slot->width_bits > 0) {
      if (slot->width_bits % 8 != 0) {
        fail(checker, field->byte_order_line, field->byte_order_column, 10,
             "'%s.%s' is %llu bits, so it has no bytes to reorder and 'byte_order' cannot apply",
             packet->name, field->name, (unsigned long long)slot->width_bits);
      } else if (offset_known && offset % 8 != 0) {
        fail(checker, field->byte_order_line, field->byte_order_column, 10,
             "'%s.%s' starts at bit %llu, which is not a byte boundary, so 'byte_order' cannot "
             "apply",
             packet->name, field->name, (unsigned long long)offset);
      }
    }

    if (offset_known && !variable && slot->width_bits > 0) {
      if (field->has_fixed && slot->width_bits <= 64) {
        discriminator_bits = offset + slot->width_bits;
      }
      offset += slot->width_bits;
    } else {
      offset_known = false;
    }

    layout->reserved_bits += slot->reserved_bits;
  }

  /* Check 11. */
  if (!has_fixed && packet->field_count > 0) {
    fail(checker, packet->line, packet->column, 11,
         "'%s' has no field with a fixed value, so nothing identifies it; a decoder handed a buffer "
         "has no way to conclude the frame is this packet",
         packet->name);
  }

  /* Second pass: project the fixed fields that sit at a constant offset. */
  layout->discriminator_bits = (size_t)discriminator_bits;
  if (discriminator_bits > 0) {
    size_t bytes = (size_t)((discriminator_bits + 7) / 8);
    layout->mask = calloc(bytes, 1);
    layout->value = calloc(bytes, 1);
    if (layout->mask == NULL || layout->value == NULL) {
      fail(checker, packet->line, packet->column, 0, "out of memory");
      return;
    }

    for (size_t i = 0; i < packet->field_count; ++i) {
      const AstField *field = &packet->fields[i];
      const FieldLayout *slot = &layout->fields[i];
      if (!field->has_fixed || slot->variable || !slot->offset_known || slot->width_bits == 0 ||
          slot->width_bits > 64) {
        continue;
      }
      if (slot->offset_bits + slot->width_bits > discriminator_bits) {
        continue;
      }
      project(field, slot->offset_bits, slot->width_bits, layout->mask, layout->value);
    }
  }
}

/* ---- Check 12 -------------------------------------------------------------------------------
 *
 * Two packets are distinguishable when they disagree somewhere both constrain. Having different
 * discriminators is not enough: if they sit at offsets that do not overlap, the masks share no bits,
 * nothing contradicts, and a frame can satisfy both. */
static void check_distinguishable(Checker *checker) {
  const Layout *layout = checker->layout;

  for (size_t a = 0; a < layout->packet_count; ++a) {
    for (size_t b = a + 1; b < layout->packet_count; ++b) {
      const PacketLayout *lhs = &layout->packets[a];
      const PacketLayout *rhs = &layout->packets[b];

      size_t shared_bits = lhs->discriminator_bits < rhs->discriminator_bits
                               ? lhs->discriminator_bits
                               : rhs->discriminator_bits;
      size_t shared_bytes = (shared_bits + 7) / 8;

      bool contradicts = false;
      for (size_t i = 0; i < shared_bytes && !contradicts; ++i) {
        uint8_t both = (uint8_t)(lhs->mask[i] & rhs->mask[i]);
        uint8_t differ = (uint8_t)(lhs->value[i] ^ rhs->value[i]);
        if ((both & differ) != 0) {
          contradicts = true;
        }
      }

      if (!contradicts) {
        const AstPacket *packet = &checker->file->packets[b];
        fail(checker, packet->line, packet->column, 12,
             "'%s' and '%s' cannot be told apart: no bit is fixed to different values in both, so a "
             "frame could satisfy either",
             checker->file->packets[a].name, checker->file->packets[b].name);
      }
    }
  }
}

Layout *check_file(const AstFile *file, DiagList *diags) {
  Layout *layout = calloc(1, sizeof(Layout));
  if (layout == NULL) {
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  layout->packets = calloc(file->packet_count == 0 ? 1 : file->packet_count, sizeof(PacketLayout));
  if (layout->packets == NULL) {
    free(layout);
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }
  layout->packet_count = file->packet_count;

  Checker checker = {.file = file, .diags = diags, .layout = layout, .failed = false};

  /* Check 1. */
  for (size_t i = 0; i < file->packet_count; ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(file->packets[j].name, file->packets[i].name) == 0) {
        fail(&checker, file->packets[i].line, file->packets[i].column, 1,
             "there is already a packet called '%s'", file->packets[i].name);
        break;
      }
    }
  }

  for (size_t i = 0; i < file->packet_count; ++i) {
    check_packet(&checker, i);
  }

  /* Only worth asking once every packet's own fixed fields are known to be sound: a packet that
   * failed check 8 or 11 has a projection nobody should draw conclusions from. */
  if (!checker.failed) {
    check_distinguishable(&checker);
  }

  diag_sort(diags);

  if (checker.failed) {
    layout_free(layout);
    return NULL;
  }
  return layout;
}

void layout_free(Layout *layout) {
  if (layout == NULL) {
    return;
  }
  for (size_t i = 0; i < layout->packet_count; ++i) {
    free(layout->packets[i].fields);
    free(layout->packets[i].mask);
    free(layout->packets[i].value);
  }
  free(layout->packets);
  free(layout);
}
