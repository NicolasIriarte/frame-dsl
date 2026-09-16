#include <string.h>

#include "bytecode.h"
#include "definition.h"
#include "frame.h"
#include "framedsl.h"

/* Turning a frame's values into bytes.
 *
 * The walk is a loop over the packet's field instructions. Per field it works out a width, reads a
 * value from the arena -- or, for a fixed field, from the definition -- and lays it into the output
 * buffer at the bit offset it has reached. There is no tree, no recursion and no allocation.
 *
 * Bit numbering is the one thing worth stating precisely, because everything else follows from it:
 *
 *     stream bit i is bit 7 - (i % 8) of byte i / 8
 *
 * Bit zero of a frame is the most significant bit of its first byte, which is the order the frame is
 * transmitted in. The checker's mask and value use the same numbering, so a packet's discriminator is
 * literally the bytes a frame of it must have. */

static void write_bit(uint8_t *out, uint64_t index, bool set) {
  uint8_t mask = (uint8_t)(1u << (7 - (index % 8)));
  if (set) {
    out[index / 8] |= mask;
  } else {
    out[index / 8] = (uint8_t)(out[index / 8] & ~mask);
  }
}

/* Lays `width` bits of `value` into the stream at `offset`.
 *
 * `msb_first` puts the value's most significant bit at the earliest stream bit the field occupies;
 * `lsb_first` puts its least significant bit there. They are genuinely different placements rather
 * than one being the other read backwards, which is why both exist. */
static void write_scalar(
  uint8_t *out,
  uint64_t offset,
  uint64_t width,
  uint64_t value,
  BitOrder bit_order,
  ByteOrder byte_order
) {
  /* byte_order reorders the bytes of a value, so it only means anything when there are whole bytes
   * sitting on byte boundaries. Check 10 rejects anyone writing it where that is untrue; inherited,
   * it simply does not apply here. */
  if (byte_order == BYTE_ORDER_LITTLE_ENDIAN && width % 8 == 0 && offset % 8 == 0) {
    uint64_t swapped = 0;
    for (uint64_t byte = 0; byte < width / 8; ++byte) {
      swapped = (swapped << 8) | ((value >> (byte * 8)) & 0xFF);
    }
    value = swapped;
  }

  for (uint64_t j = 0; j < width; ++j) {
    uint64_t bit = bit_order == BIT_ORDER_MSB_FIRST ? (value >> (width - 1 - j)) & 1 : (value >> j) & 1;
    write_bit(out, offset + j, bit != 0);
  }
}

/* A bit string goes out as the bytes it was given, each most significant bit first, starting wherever
 * the field sits. Neither attribute applies to it -- check 15 rejects writing one on a bit-string
 * field -- because these are bytes somebody handed over rather than a number with an order. */
static void write_bytes(uint8_t *out, uint64_t offset, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    for (uint64_t j = 0; j < 8; ++j) {
      write_bit(out, offset + i * 8 + j, ((bytes[i] >> (7 - j)) & 1) != 0);
    }
  }
}

/* Reads the packet's scalars into the flat array a width program indexes.
 *
 * Every slot, not just the ones referenced: the program indexes by field number, and working out
 * which are needed would cost more than filling an array that is a handful of entries long. A
 * non-scalar contributes nothing and cannot be referenced anyway -- check 16 sees to that. */
static void gather_slots(const FramedslFrame *frame, size_t packet, size_t count, uint64_t *slots) {
  size_t base = frame->packet_base[packet];
  for (size_t i = 0; i < count; ++i) {
    const Slot *slot = &frame->slots[base + i];
    if (slot->is_scalar) {
      memcpy(&slots[i], frame->arena + slot->offset, sizeof(uint64_t));
    } else {
      slots[i] = 0;
    }
  }
}

/* The width this field has in this frame, in bits. */
static bool resolve_width(
  const FramedslFrame *frame,
  const FieldInstruction *instruction,
  const uint64_t *slots,
  uint64_t *width,
  FramedslStatus *status
) {
  (void)frame;

  if (!instruction->variable) {
    *width = instruction->width_bits;
    return true;
  }

  if (!width_program_eval(&instruction->width, slots, width)) {
    /* Overflow, a division by zero, or a subtraction below zero -- all of which depend on the
     * frame's own contents and so could not have been settled when the file loaded. */
    *status = FRAMEDSL_ERROR_RANGE;
    return false;
  }
  if (*width > instruction->max_bits) {
    *status = FRAMEDSL_ERROR_RANGE;
    return false;
  }
  return true;
}

const uint8_t *framedsl_encode(FramedslFrame *frame, const char *packet, size_t *length, FramedslStatus *status) {
  FramedslStatus ignored = FRAMEDSL_OK;
  if (status == NULL) {
    status = &ignored;
  }
  *status = FRAMEDSL_OK;

  if (frame == NULL || length == NULL) {
    *status = FRAMEDSL_ERROR_MEMORY;
    return NULL;
  }

  size_t index = 0;
  if (packet == NULL || !framedsl_find_packet(frame->definition, packet, &index)) {
    *status = FRAMEDSL_ERROR_FIELD;
    return NULL;
  }

  const PacketProgram *program = &frame->definition->program->packets[index];
  const size_t base = frame->packet_base[index];

  /* Cleared rather than written over, so a field narrower than the one a previous encode put there
   * cannot leave a bit of it behind. */
  memset(frame->output, 0, frame->output_size);

  uint64_t slots[FRAMEDSL_MAX_FIELDS_PER_PACKET];
  if (program->field_count > FRAMEDSL_MAX_FIELDS_PER_PACKET) {
    *status = FRAMEDSL_ERROR_RANGE;
    return NULL;
  }
  gather_slots(frame, index, program->field_count, slots);

  uint64_t offset = 0;
  for (size_t f = 0; f < program->field_count; ++f) {
    const FieldInstruction *instruction = &program->fields[f];
    const Slot *slot = &frame->slots[base + f];

    uint64_t width = 0;
    if (!resolve_width(frame, instruction, slots, &width, status)) {
      return NULL;
    }

    if (instruction->has_fixed) {
      /* From the definition, never from the arena: a caller cannot emit a frame that fails to
       * identify itself, whatever they put in the slot. */
      write_scalar(frame->output, offset, width, instruction->fixed, instruction->bit_order, instruction->byte_order);
    } else if (instruction->is_scalar) {
      write_scalar(frame->output, offset, width, slots[f], instruction->bit_order, instruction->byte_order);
    } else {
      /* A variable field's width comes from the expression, and the bytes come from the arena. Those
       * two can disagree -- a caller who sets `len` to 12 and then twenty bytes of `data` has
       * described two different frames -- and the definition is what decides, so the disagreement is
       * an error rather than a silent truncation. */
      if (width != (uint64_t)slot->length * 8) {
        *status = FRAMEDSL_ERROR_RANGE;
        return NULL;
      }
      write_bytes(frame->output, offset, frame->arena + slot->offset, slot->length);
    }

    offset += width;
  }

  /* Check 13 makes this true for every packet that loaded, so a frame is always whole bytes and the
   * caller never needs a bit count alongside the buffer. Checked rather than assumed, because it is
   * the one place a bug in the check would show up as a frame that is silently one bit short. */
  if (offset % 8 != 0) {
    *status = FRAMEDSL_ERROR_RANGE;
    return NULL;
  }

  *length = (size_t)(offset / 8);
  return frame->output;
}

/* The same encode, into a buffer the caller owns.
 *
 * It builds the frame in the frame's own output buffer first and copies. That looks like a copy too
 * many until you ask what the alternative is: the length of a frame with a valof() field is not known
 * until the walk that produces it has finished, so encoding straight into the caller's buffer means
 * either writing into it before knowing whether it is big enough, or walking the widths twice. The
 * first breaks the promise that a refused call leaves the buffer alone -- which is the promise a
 * caller with a transmit ring actually needs -- and the second costs more than a memcpy of a frame.
 *
 * A NULL buffer with a capacity of zero is the useful degenerate case: it is how a caller asks how
 * big the frame would be, and it answers without writing anywhere. */
FramedslStatus framedsl_encode_into(
  FramedslFrame *frame,
  const char *packet,
  uint8_t *buffer,
  size_t capacity,
  size_t *length
) {
  if (length == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  *length = 0;

  size_t encoded = 0;
  FramedslStatus status = FRAMEDSL_OK;
  const uint8_t *bytes = framedsl_encode(frame, packet, &encoded, &status);
  if (bytes == NULL) {
    return status;
  }

  /* Reported whether or not it fits, so a caller sizes the next attempt from the answer. */
  *length = encoded;

  if (capacity < encoded || (buffer == NULL && encoded > 0)) {
    return FRAMEDSL_ERROR_BUFFER;
  }

  memcpy(buffer, bytes, encoded);
  return FRAMEDSL_OK;
}
