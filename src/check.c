#include "check.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A width no sane frame reaches, used to stop a fold from running away rather than to express a real
 * limit. 2^32 bits is half a gigabyte in one field. */
#define WIDTH_CEILING ((int64_t)1 << 32)

/* A field count no sane packet reaches, block expansion included. Found by fuzzing (#59): several
 * checks below -- duplicate names foremost -- are quadratic in field count, which is fine for a real
 * frame's dozens of fields and is a multi-minute hang for a fuzzer-sized packet with hundreds of
 * thousands of one-bit fields. Checked before any of them runs, so the packet that would make them slow
 * is rejected in the time check 4 already takes. */
#define FIELD_CEILING 4096

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
    case '+':
      *out = lhs + rhs;
      break;
    case '-':
      *out = lhs - rhs;
      break;
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
    default:
      return FOLD_NOT_CONSTANT;
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

/* ---- Byte-multiple analysis (check 13) ------------------------------------------------------
 *
 * A frame leaves this library as bytes, so a packet that does not land on a byte boundary has no
 * unambiguous byte form -- its last byte would be part frame and part nothing. Sub-byte *fields* are
 * untouched by this; they are what the language exists for. What check 13 requires is that a
 * packet's fields *sum* to a multiple of 8.
 *
 * A valof() width makes that sum depend on the frame, so it is decided by evaluating widths
 * abstractly over Z/8 rather than by adding them. A literal is known; a valof() is not; a product is
 * zero when either side is a known multiple of eight, whatever the other side is; a sum or a
 * difference is known when both sides are. Anything that folds to a constant is known exactly,
 * which covers division without a rule of its own.
 *
 * It is conservative: it needs one factor that is itself a multiple of eight, so `valof(n) * 2 * 4`
 * is rejected even though it always is one. That is the right direction to be wrong in -- the
 * alternative is a width that is only found to be unusable when a frame arrives. */
static bool residue_mod8(const Expr *expr, uint64_t *out) {
  if (expr == NULL) {
    return false;
  }

  int64_t constant = 0;
  if (fold(expr, &constant) == FOLD_OK) {
    *out = (uint64_t)(((constant % 8) + 8) % 8);
    return true;
  }

  /* Not constant and not a binary operator is a bare valof(): nothing is known about it. */
  if (expr->kind != EXPR_BINARY) {
    return false;
  }

  uint64_t lhs = 0;
  uint64_t rhs = 0;
  bool lhs_known = residue_mod8(expr->lhs, &lhs);
  bool rhs_known = residue_mod8(expr->rhs, &rhs);

  switch (expr->op) {
  case '+':
    if (!lhs_known || !rhs_known) {
      return false;
    }
    *out = (lhs + rhs) % 8;
    return true;

  case '-':
    if (!lhs_known || !rhs_known) {
      return false;
    }
    *out = (lhs - rhs + 8) % 8;
    return true;

  case '*':
    /* One byte-multiple factor is enough: 8m * k is a multiple of eight for every k. */
    if ((lhs_known && lhs == 0) || (rhs_known && rhs == 0)) {
      *out = 0;
      return true;
    }
    if (!lhs_known || !rhs_known) {
      return false;
    }
    *out = (lhs * rhs) % 8;
    return true;

  /* Division does not distribute over the modulus, so only the folded case above can answer it. */
  default:
    return false;
  }
}

/* A "did you mean" for a valof() naming nothing: the closest field declared before this one. Inside a
 * block instance every name carries the instance's prefix -- `hdr.lenght` -- but what is written in
 * the parentheses is only the part after it, so the prefix is set aside for the comparison and the
 * replacement. */
static void suggest_reference(
  Checker *checker,
  size_t index,
  const AstPacket *packet,
  size_t field_index,
  const Expr *expr
) {
  if (expr->reference_line == 0) {
    return;
  }
  const char *dot = strrchr(expr->reference, '.');
  const size_t prefix = dot != NULL ? (size_t)(dot - expr->reference) + 1 : 0;
  const char *written = expr->reference + prefix;

  const char **names = malloc((field_index + 1) * sizeof(*names));
  if (names == NULL) {
    return;
  }
  size_t count = 0;
  for (size_t i = 0; i < field_index; ++i) {
    const char *name = packet->fields[i].name;
    if (strncmp(name, expr->reference, prefix) == 0 && strchr(name + prefix, '.') == NULL) {
      names[count++] = name + prefix;
    }
  }
  const char *closest = diag_closest(written, strlen(written), names, count);
  if (closest != NULL) {
    diag_set_fix(
      checker->diags,
      index,
      expr->reference_line,
      expr->reference_column,
      expr->reference_line,
      expr->reference_column + (int)strlen(written),
      closest,
      "did you mean '%s'?",
      closest
    );
  }
  free(names);
}

/* The same for a name that should be a calibration's, written at (line, column). */
static void suggest_cal(Checker *checker, size_t index, const AstFile *file, const char *wrong, int line, int column) {
  const char **names = malloc((file->cal_count + 1) * sizeof(*names));
  if (names == NULL || line == 0) {
    free(names);
    return;
  }
  for (size_t i = 0; i < file->cal_count; ++i) {
    names[i] = file->cals[i].name;
  }
  const char *closest = diag_closest(wrong, strlen(wrong), names, file->cal_count);
  if (closest != NULL) {
    diag_set_fix(
      checker->diags, index, line, column, line, column + (int)strlen(wrong), closest, "did you mean '%s'?", closest
    );
  }
  free(names);
}

/* Check 7: a valof() may only name a field declared earlier in the same packet. Reading a field
 * means knowing where it starts, which means every field before it is already resolved. */
static void check_references(
  Checker *checker,
  const PacketLayout *layout,
  const AstPacket *packet,
  size_t field_index,
  const Expr *expr
) {
  if (expr == NULL) {
    return;
  }
  if (expr->kind == EXPR_BINARY) {
    check_references(checker, layout, packet, field_index, expr->lhs);
    check_references(checker, layout, packet, field_index, expr->rhs);
    return;
  }
  if (expr->kind != EXPR_VALOF) {
    return;
  }

  for (size_t i = 0; i < field_index; ++i) {
    if (strcmp(packet->fields[i].name, expr->reference) == 0) {
      /* Check 16. A width is arithmetic on a number, and only a scalar is one -- anything wider is a
       * run of bytes with no single value to multiply. Caught here rather than at encode time, where
       * it would be a width computed from whatever the first eight bytes of a payload happened to
       * be. */
      const FieldLayout *referenced = &layout->fields[i];
      if (!referenced->is_scalar) {
        fail(
          checker,
          expr->line,
          expr->column,
          16,
          "valof(%s) names a field of more than %d bits, which has no single value to compute a "
          "width from; only a scalar can size another field",
          expr->reference,
          FRAMEDSL_SCALAR_MAX_BITS
        );
      }
      return;
    }
  }

  /* Distinguish "later in this packet" from "nowhere here": they are different mistakes, and the
   * second is usually a field belonging to another packet. */
  for (size_t i = field_index; i < packet->field_count; ++i) {
    if (strcmp(packet->fields[i].name, expr->reference) == 0) {
      fail(
        checker,
        expr->line,
        expr->column,
        7,
        "valof(%s) names a field declared later in '%s'; a width can only depend on a field "
        "already read",
        expr->reference,
        packet->name
      );
      return;
    }
  }

  size_t index = checker->diags->count;
  fail(checker, expr->line, expr->column, 7, "valof(%s) names no field in '%s'", expr->reference, packet->name);
  suggest_reference(checker, index, packet, field_index, expr);
}

/* ---- Fixed-bit projection (check 12) -------------------------------------------------------- */

/* Stream bit `index` is bit 7 - index%8 of byte index/8: bit zero of a frame is the most significant
 * bit of its first byte, which is the order a frame is transmitted in.
 *
 * That numbering is not an internal convenience. It makes a packet's mask and value *the bytes a
 * frame must have*, so the decoder matches an incoming buffer against them with an AND and a
 * compare rather than reversing anything first. */
static void bits_set(uint8_t *bits, size_t index) {
  bits[index / 8] |= (uint8_t)(1u << (7 - (index % 8)));
}

/* Lays a fixed value onto the stream bits its field occupies, applying that field's bit order and
 * byte order first -- the question is which *stream* bits are pinned, not which bits of the value
 * are. */
static void project(const AstField *field, uint64_t offset, uint64_t width, uint8_t *mask, uint8_t *value) {
  uint64_t wire = field->fixed;

  if (field->byte_order == BYTE_ORDER_LITTLE_ENDIAN && width % 8 == 0 && offset % 8 == 0) {
    uint64_t swapped = 0;
    for (uint64_t byte = 0; byte < width / 8; ++byte) {
      swapped = (swapped << 8) | ((wire >> (byte * 8)) & 0xFF);
    }
    wire = swapped;
  }

  for (uint64_t j = 0; j < width; ++j) {
    uint64_t bit = field->bit_order == BIT_ORDER_MSB_FIRST ? (wire >> (width - 1 - j)) & 1 : (wire >> j) & 1;
    bits_set(mask, (size_t)(offset + j));
    if (bit != 0) {
      bits_set(value, (size_t)(offset + j));
    }
  }
}

/* ---- Per-packet checks ---------------------------------------------------------------------- */

/* Every check a run of fields can be judged by. A named block goes through here as well, so that a
 * mistake inside one is reported at the block, once, and in a block nothing uses yet -- which is what
 * an imported file mostly is. What only makes sense of a whole frame, checks 11 and 13's total, is
 * skipped for a block: a header need not be a whole number of bytes, nor identify anything, on its
 * own. */
static void check_packet(Checker *checker, const AstPacket *packet, PacketLayout *layout, bool is_block) {
  /* Check 29, ahead of even the allocation below: once a packet is this big, the quadratic checks
   * further down -- duplicate names foremost -- are the actual cost, and there is no reason to pay it,
   * or size the layout for it, just to report the same rejection more slowly. */
  if (packet->field_count > FIELD_CEILING) {
    fail(
      checker,
      packet->line,
      packet->column,
      29,
      "'%s' has %zu fields, more than a %s may ever have",
      packet->name,
      packet->field_count,
      is_block ? "block" : "packet"
    );
    return;
  }

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

  /* Check 13 accumulates the packet's width modulo eight rather than its width, because a valof()
   * field has no width until a frame arrives but can still be known to be a whole number of bytes. */
  uint64_t byte_residue = 0;
  bool byte_residue_known = true;

  /* First pass: widths, offsets, and everything decidable field by field. */
  for (size_t i = 0; i < packet->field_count; ++i) {
    const AstField *field = &packet->fields[i];
    FieldLayout *slot = &layout->fields[i];

    /* Check 2. */
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(packet->fields[j].name, field->name) == 0) {
        fail(checker, field->line, field->column, 2, "'%s' already has a field called '%s'", packet->name, field->name);
        break;
      }
    }

    check_references(checker, layout, packet, i, field->width);
    check_references(checker, layout, packet, i, field->reserve);

    bool variable = expr_uses_valof(field->width);
    slot->variable = variable;
    bool width_ok = true;

    if (variable) {
      /* Check 5. */
      if (field->reserve == NULL) {
        fail(
          checker,
          field->line,
          field->column,
          5,
          "'%s.%s' has a width that depends on another field, so it must reserve a slot: write "
          "'%s[<bytes>]'. Without one it cannot be given a fixed place in the arena",
          packet->name,
          field->name,
          field->name
        );
      }
    } else {
      /* Check 4. */
      int64_t width = 0;
      FoldResult folded = fold(field->width, &width);
      if (folded == FOLD_DIVIDE_BY_ZERO) {
        fail(
          checker, field->line, field->column, 4, "'%s.%s' has a width that divides by zero", packet->name, field->name
        );
        width_ok = false;
      } else if (folded == FOLD_TOO_LARGE) {
        fail(
          checker, field->line, field->column, 4, "'%s.%s' has an impossibly large width", packet->name, field->name
        );
        width_ok = false;
      } else if (folded != FOLD_OK || width <= 0) {
        fail(
          checker,
          field->line,
          field->column,
          4,
          "'%s.%s' must have a width of at least one bit",
          packet->name,
          field->name
        );
        width_ok = false;
      } else {
        slot->width_bits = (uint64_t)width;
      }
    }

    /* Check 13, field by field. A width nobody can show to be a whole number of bytes makes the
     * packet's own length depend on the frame it is carrying, which is exactly what the packet-wide
     * half of this check is there to prevent. Reported here, against the field responsible, rather
     * than as a packet that mysteriously does not add up. */
    if (!width_ok) {
      byte_residue_known = false;
    } else {
      uint64_t residue = 0;
      if (!residue_mod8(field->width, &residue)) {
        fail(
          checker,
          field->line,
          field->column,
          13,
          "'%s.%s' has a width that cannot be shown to be a whole number of bytes, so '%s' would "
          "be a different length for every frame it carries; write the width as a multiple of 8 "
          "bits, as in 'valof(x) * 8'",
          packet->name,
          field->name,
          packet->name
        );
        byte_residue_known = false;
      } else {
        byte_residue = (byte_residue + residue) % 8;
      }
    }

    /* Check 6. The reservation is written in bytes -- the only number in the language that is --
     * because it is the size of a slot rather than a position in a frame. It becomes bits here, and
     * nothing downstream of this line sees bytes again. */
    if (field->reserve != NULL) {
      int64_t bound = 0;
      FoldResult folded = fold(field->reserve, &bound);
      if (folded == FOLD_NOT_CONSTANT) {
        fail(
          checker,
          field->reserve->line,
          field->reserve->column,
          6,
          "'%s.%s' reserves a size that depends on another field; a reservation must be a "
          "constant, since it is what the arena sets aside before any frame arrives",
          packet->name,
          field->name
        );
      } else if (folded != FOLD_OK || bound <= 0) {
        fail(
          checker,
          field->reserve->line,
          field->reserve->column,
          6,
          "'%s.%s' must reserve at least one byte",
          packet->name,
          field->name
        );
      } else if ((uint64_t)bound > (uint64_t)WIDTH_CEILING / 8) {
        /* Found by fuzzing (#59): fold()'s own ceiling bounds a literal to WIDTH_CEILING, but that is
         * meant as bits -- applied to a reservation, which is bytes, it let a field ask the arena for
         * up to 4GiB instead of the half-gigabyte every other width is held to. */
        fail(
          checker,
          field->reserve->line,
          field->reserve->column,
          6,
          "'%s.%s' reserves %lld bytes, more than any field may ever reserve",
          packet->name,
          field->name,
          (long long)bound
        );
      } else {
        slot->max_bits = (uint64_t)bound * 8;
      }
    }

    /* Check 14: a reservation says how much room a field needs when nobody can know until a frame
     * arrives. A constant-width field already answers that with its width, and a second number could
     * only ever agree with it or contradict it. */
    if (width_ok && !variable && field->reserve != NULL) {
      fail(
        checker,
        field->reserve_line,
        field->reserve_column,
        14,
        "'%s.%s' is %llu bits wide, so its slot is already known and it must not reserve one; "
        "a reservation belongs only on a field whose width depends on another field",
        packet->name,
        field->name,
        (unsigned long long)slot->width_bits
      );
    }

    /* Check 9. */
    if (field->has_fixed && (variable || field->reserve != NULL)) {
      fail(
        checker,
        field->fixed_line,
        field->fixed_column,
        9,
        "'%s.%s' cannot have both a fixed value and a reservation: a width that is not known "
        "until a frame arrives cannot identify the packet that frame belongs to",
        packet->name,
        field->name
      );
    }

    slot->reserved_bits = variable ? slot->max_bits : slot->width_bits;
    /* A repetition is elements, not one number, however few bits two of them come to (#55). */
    slot->is_scalar =
      !variable && slot->width_bits > 0 && slot->width_bits <= FRAMEDSL_SCALAR_MAX_BITS && field->element == NULL;
    slot->offset_known = offset_known;
    slot->offset_bits = offset_known ? offset : 0;

    /* Check 8. A fixed value is a number, stored and compared as one; a field wider than a scalar is a
     * bit string, the bytes the caller handed over, with no single value to hold one against. Found by
     * fuzzing (#59): nothing stopped a fixed value from being written on one, and encoding it read past
     * the 64 bits `field->fixed` actually has. */
    if (field->has_fixed) {
      has_fixed = true;
      uint64_t width = slot->width_bits;
      if (!variable && width > FRAMEDSL_SCALAR_MAX_BITS) {
        fail(
          checker,
          field->fixed_line,
          field->fixed_column,
          8,
          "'%s.%s' is %llu bits, wider than a scalar's %d -- a fixed value cannot be given to a bit "
          "string",
          packet->name,
          field->name,
          (unsigned long long)width,
          FRAMEDSL_SCALAR_MAX_BITS
        );
      } else if (!variable && width > 0 && width < 64 && field->fixed >= ((uint64_t)1 << width)) {
        fail(
          checker,
          field->fixed_line,
          field->fixed_column,
          8,
          "'%s.%s' is %llu bits, which cannot hold the value %#llx",
          packet->name,
          field->name,
          (unsigned long long)width,
          (unsigned long long)field->fixed
        );
      }
    }

    /* Check 15. Both attributes describe how the bits of a *value* are arranged. A bit string is not
     * a value -- it is the bytes the caller handed over, already in whatever order the thing that
     * produced them wrote, and the library has no basis to reorder an opaque payload. Written here it
     * can do nothing, which is the same reason check 10 exists. Inherited, it is simply irrelevant to
     * this field and meaningful to the next one. */
    if (width_ok && !slot->is_scalar && (field->bit_order_written || field->byte_order_written)) {
      bool byte = field->byte_order_written;
      fail(
        checker,
        byte ? field->byte_order_line : field->bit_order_line,
        byte ? field->byte_order_column : field->bit_order_column,
        15,
        "'%s.%s' is a bit string, not a number, so '%s' cannot apply to it; it is written to the "
        "frame as the bytes it was given",
        packet->name,
        field->name,
        byte ? "byte_order" : "bit_order"
      );
    }

    /* Check 10. Only a byte_order *written here* is an error: one inherited from packet or file
     * scope covers every field below it, and most real frames mix byte-wide fields with sub-byte
     * ones. What is worth rejecting is somebody writing a setting that cannot do anything. */
    if (field->byte_order_written && slot->is_scalar && slot->width_bits > 0) {
      if (slot->width_bits % 8 != 0) {
        fail(
          checker,
          field->byte_order_line,
          field->byte_order_column,
          10,
          "'%s.%s' is %llu bits, so it has no bytes to reorder and 'byte_order' cannot apply",
          packet->name,
          field->name,
          (unsigned long long)slot->width_bits
        );
      } else if (offset_known && offset % 8 != 0) {
        fail(
          checker,
          field->byte_order_line,
          field->byte_order_column,
          10,
          "'%s.%s' starts at bit %llu, which is not a byte boundary, so 'byte_order' cannot "
          "apply",
          packet->name,
          field->name,
          (unsigned long long)offset
        );
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

  /* Check 13, over the packet. Only asked when every width was sound: a packet that already failed
   * check 4 does not need to be told it does not add up as well. */
  if (!is_block && byte_residue_known && packet->field_count > 0 && byte_residue != 0) {
    if (offset_known) {
      /* Every width was constant, so the exact total is worth reporting -- the author should not
       * have to add up their own fields to find out how far off they are. */
      fail(
        checker,
        packet->line,
        packet->column,
        13,
        "'%s' is %llu bits, which is not a whole number of bytes; a packet must be a multiple of "
        "8 bits so that a frame of it has an unambiguous byte form",
        packet->name,
        (unsigned long long)offset
      );
    } else {
      fail(
        checker,
        packet->line,
        packet->column,
        13,
        "'%s' is not a whole number of bytes: its fields leave %llu bit(s) over. A packet must "
        "be a multiple of 8 bits so that a frame of it has an unambiguous byte form",
        packet->name,
        (unsigned long long)byte_residue
      );
    }
  }

  /* Check 11. */
  if (!is_block && !has_fixed && packet->field_count > 0) {
    fail(
      checker,
      packet->line,
      packet->column,
      11,
      "'%s' has no field with a fixed value, so nothing identifies it; a decoder handed a buffer "
      "has no way to conclude the frame is this packet",
      packet->name
    );
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
      if (!field->has_fixed || slot->variable || !slot->offset_known || slot->width_bits == 0
          || slot->width_bits > 64) {
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

      size_t shared_bits =
        lhs->discriminator_bits < rhs->discriminator_bits ? lhs->discriminator_bits : rhs->discriminator_bits;
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
        fail(
          checker,
          packet->line,
          packet->column,
          12,
          "'%s' and '%s' cannot be told apart: no bit is fixed to different values in both, so a "
          "frame could satisfy either",
          checker->file->packets[a].name,
          checker->file->packets[b].name
        );
      }
    }
  }
}

/* ---- Calibrations (checks 17 to 22) --------------------------------------------------------- */

static const AstCal *find_cal(const AstFile *file, const char *name, size_t *index) {
  for (size_t i = 0; i < file->cal_count; ++i) {
    if (strcmp(file->cals[i].name, name) == 0) {
      if (index != NULL) {
        *index = i;
      }
      return &file->cals[i];
    }
  }
  return NULL;
}

/* A subtree's value, when it has no name and no call in it -- which is what check 22 needs to see a
 * division by zero that no raw value could avoid. */
static bool cal_constant(const CalExpr *expr, double *out) {
  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    *out = expr->number;
    return true;
  case CAL_EXPR_NEGATE:
    if (!cal_constant(expr->lhs, out)) {
      return false;
    }
    *out = -*out;
    return true;
  case CAL_EXPR_BINARY: {
    double lhs;
    double rhs;
    if (!cal_constant(expr->lhs, &lhs) || !cal_constant(expr->rhs, &rhs)) {
      return false;
    }
    switch (expr->op) {
    case '+':
      *out = lhs + rhs;
      break;
    case '-':
      *out = lhs - rhs;
      break;
    case '*':
      *out = lhs * rhs;
      break;
    default:
      *out = rhs == 0 ? 0 : lhs / rhs;
      break;
    }
    return true;
  }
  default:
    return false;
  }
}

/* Checks 18, 19 and 22 over one body. `param` is the only bare name it may use. */
static void check_cal_body(Checker *checker, const AstCal *cal, const char *param, const CalExpr *expr) {
  if (expr == NULL) {
    return;
  }

  switch (expr->kind) {
  case CAL_EXPR_NUMBER:
    return;
  case CAL_EXPR_NAME:
    if (strcmp(expr->name, param) != 0) {
      fail(
        checker,
        expr->line,
        expr->column,
        19,
        "'%s' in calibration '%s' is not its parameter '%s'; a calibration sees only the value it is "
        "given",
        expr->name,
        cal->name,
        param
      );
    }
    return;
  case CAL_EXPR_CALL:
    if (find_cal(checker->file, expr->name, NULL) == NULL) {
      size_t index = checker->diags->count;
      fail(checker, expr->line, expr->column, 18, "'%s' calls '%s', which is not a calibration", cal->name, expr->name);
      suggest_cal(checker, index, checker->file, expr->name, expr->line, expr->column);
    }
    check_cal_body(checker, cal, param, expr->lhs);
    return;
  case CAL_EXPR_NEGATE:
    check_cal_body(checker, cal, param, expr->lhs);
    return;
  case CAL_EXPR_BINARY: {
    check_cal_body(checker, cal, param, expr->lhs);
    check_cal_body(checker, cal, param, expr->rhs);
    double divisor;
    if (expr->op == '/' && cal_constant(expr->rhs, &divisor) && divisor == 0) {
      fail(checker, expr->rhs->line, expr->rhs->column, 22, "calibration '%s' divides by a constant zero", cal->name);
    }
    return;
  }
  }
}

/* Check 20: whether `target` can be reached again from the calls in `expr`, by depth-first search.
 * Asked per calibration, so a curve that only calls *into* somebody else's cycle is not blamed for it.
 * Only a *forward* body's calls count: an inverse may call its own forward curve, which is a
 * different function rather than recursion. `seen` stops the walk revisiting a curve. */
static bool reaches(const AstFile *file, const CalExpr *expr, size_t target, uint8_t *seen) {
  if (expr == NULL) {
    return false;
  }
  if (expr->kind == CAL_EXPR_CALL) {
    size_t callee;
    if (find_cal(file, expr->name, &callee) != NULL) {
      if (callee == target) {
        return true;
      }
      if (!seen[callee]) {
        seen[callee] = 1;
        if (reaches(file, file->cals[callee].body, target, seen)) {
          return true;
        }
      }
    }
  }
  return reaches(file, expr->lhs, target, seen) || reaches(file, expr->rhs, target, seen);
}

/* Checks 18 and 21 from the fields' side: the calibration a field names exists, and the field is a
 * number. `layout` is the one check_packet() filled in for `packet`, packet or block. */
static void check_field_cals(Checker *checker, const AstPacket *packet, const PacketLayout *layout) {
  const AstFile *file = checker->file;
  if (layout->fields == NULL) {
    return; /* its allocation failed, and check_packet() said so */
  }
  for (size_t f = 0; f < packet->field_count; ++f) {
    const AstField *field = &packet->fields[f];
    if (field->cal == NULL) {
      continue;
    }

    /* Check 18, from the field's side. */
    if (find_cal(file, field->cal, NULL) == NULL) {
      size_t index = checker->diags->count;
      fail(
        checker,
        field->cal_line,
        field->cal_column,
        18,
        "'%s.%s' names the calibration '%s', and there is none",
        packet->name,
        field->name,
        field->cal
      );
      suggest_cal(checker, index, file, field->cal, field->cal_value_line, field->cal_value_column);
    }

    /* Check 21. Asked of the width directly rather than of is_scalar, which a width that failed
     * check 4 leaves unset -- and that failure has been reported already. */
    const FieldLayout *slot = &layout->fields[f];
    if (slot->variable || slot->width_bits > FRAMEDSL_SCALAR_MAX_BITS || field->element != NULL) {
      fail(
        checker,
        field->cal_line,
        field->cal_column,
        21,
        "'%s.%s' is a bit string, not a number, so a calibration cannot apply to it",
        packet->name,
        field->name
      );
    }
  }
}

static void check_cals(Checker *checker) {
  const AstFile *file = checker->file;

  for (size_t i = 0; i < file->cal_count; ++i) {
    const AstCal *cal = &file->cals[i];

    /* Check 17. */
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(file->cals[j].name, cal->name) == 0) {
        fail(checker, cal->line, cal->column, 17, "there is already a calibration called '%s'", cal->name);
        break;
      }
    }

    check_cal_body(checker, cal, cal->param, cal->body);
    if (cal->inverse != NULL) {
      check_cal_body(checker, cal, cal->inverse_param, cal->inverse);
    }
  }

  if (file->cal_count > 0) {
    uint8_t *state = calloc(file->cal_count, 1);
    if (state == NULL) {
      fail(checker, 1, 1, 0, "out of memory");
      return;
    }
    for (size_t i = 0; i < file->cal_count; ++i) {
      memset(state, 0, file->cal_count);
      if (reaches(file, file->cals[i].body, i, state)) {
        fail(
          checker,
          file->cals[i].line,
          file->cals[i].column,
          20,
          "calibration '%s' calls itself, directly or through another; a curve has to finish",
          file->cals[i].name
        );
      }
    }
    free(state);
  }

  for (size_t p = 0; p < file->packet_count; ++p) {
    check_field_cals(checker, &file->packets[p], &checker->layout->packets[p]);
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

  Checker checker = { .file = file, .diags = diags, .layout = layout, .failed = false };

  /* Check 1. */
  for (size_t i = 0; i < file->packet_count; ++i) {
    for (size_t j = 0; j < i; ++j) {
      if (strcmp(file->packets[j].name, file->packets[i].name) == 0) {
        fail(
          &checker,
          file->packets[i].line,
          file->packets[i].column,
          1,
          "there is already a packet called '%s'",
          file->packets[i].name
        );
        break;
      }
    }
  }

  /* Named blocks first, on their own. A mistake inside one is then reported once, at the block,
   * rather than once per packet that uses it -- and the packets are only checked once every block
   * they are built from is sound. */
  PacketLayout *block_layouts = calloc(file->block_count == 0 ? 1 : file->block_count, sizeof(PacketLayout));
  if (block_layouts == NULL) {
    fail(&checker, 1, 1, 0, "out of memory");
  }
  for (size_t i = 0; i < file->block_count && block_layouts != NULL; ++i) {
    check_packet(&checker, &file->blocks[i], &block_layouts[i], true);
    check_field_cals(&checker, &file->blocks[i], &block_layouts[i]);
  }
  bool blocks_sound = !checker.failed;

  for (size_t i = 0; i < file->packet_count && blocks_sound; ++i) {
    check_packet(&checker, &file->packets[i], &layout->packets[i], false);
  }
  check_cals(&checker);

  for (size_t i = 0; i < file->block_count && block_layouts != NULL; ++i) {
    free(block_layouts[i].fields);
    free(block_layouts[i].mask);
    free(block_layouts[i].value);
  }
  free(block_layouts);

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
