#include <string.h>

#include "bits.h"
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

  /* lsb_first puts the value's bits in the opposite order to the stream's, so it is the value reversed
   * laid down msb_first (#108): the same runs, at any offset. */
  if (bit_order == BIT_ORDER_LSB_FIRST && width > 0) {
    value = bits_reverse(value, width);
  }

  /* msb_first is the value's bits in stream order, so it goes out in runs: as many as are left in the
   * current byte, at most eight at a time, whatever the alignment. The output was zeroed before the
   * walk and no two fields overlap, so a run is OR'd in rather than written bit by bit. */
  uint64_t position = offset;
  uint64_t remaining = width;
  while (remaining > 0) {
    uint64_t used = position % 8;
    uint64_t run = 8 - used < remaining ? 8 - used : remaining;
    uint64_t bits = (value >> (remaining - run)) & ((1u << run) - 1);
    out[position / 8] |= (uint8_t)(bits << (8 - used - run));
    position += run;
    remaining -= run;
  }
}

/* A bit string goes out as the bytes it was given, each most significant bit first, starting wherever
 * the field sits. Neither attribute applies to it -- check 15 rejects writing one on a bit-string
 * field -- because these are bytes somebody handed over rather than a number with an order. */
static void write_bytes(uint8_t *out, uint64_t offset, const uint8_t *bytes, size_t length) {
  uint8_t *at = out + offset / 8;
  const unsigned shift = (unsigned)(offset % 8);

  /* On a byte boundary it is a copy, which is the common case and was the benchmark's biggest cost. */
  if (shift == 0) {
    if (length > 0) {
      memcpy(at, bytes, length);
    }
    return;
  }

  /* Off one, each byte straddles two: its high bits finish the current output byte and its low bits
   * start the next. The output was zeroed before the walk, so both halves are OR'd in.
   *
   * Eight bytes at a time first (#105): eight source bytes as one big-endian word, shifted right by the
   * offset, are the same bits landing in eight output bytes, all but the last `shift` of them, which
   * start the ninth. Those are carried into the next word in a register rather than OR'd into memory,
   * so each step is one store that nothing reads back -- a byte stored and then loaded as part of a
   * word stalls the load. The first output byte starts with the bits of the field before this one.
   * Two words a step while there are two, as read_bytes() in decode.c does. */
  size_t i = 0;
  if (length >= 8) {
    const unsigned back = 8 - shift;
    uint64_t carry = at[0];
    for (; i + 16 <= length; i += 16) {
      uint64_t first = bits_load_be64(bytes + i);
      uint64_t second = bits_load_be64(bytes + i + 8);
      bits_store_be64(at + i, carry << 56 | first >> shift);
      bits_store_be64(at + i + 8, (uint64_t)(uint8_t)(first << back) << 56 | second >> shift);
      carry = (uint8_t)(second << back);
    }
    for (; i + 8 <= length; i += 8) {
      uint64_t word = bits_load_be64(bytes + i);
      bits_store_be64(at + i, carry << 56 | word >> shift);
      carry = (uint8_t)(word << back);
    }
    at[i] |= (uint8_t)carry;
  }
  for (; i < length; ++i) {
    at[i] |= (uint8_t)(bytes[i] >> shift);
    at[i + 1] |= (uint8_t)(bytes[i] << (8 - shift));
  }
}

/* Reads the packet's scalars into the flat array a width program indexes.
 *
 * Every slot, not just the ones referenced: the program indexes by field number, and working out
 * which are needed would cost more than filling an array that is a handful of entries long. A
 * non-scalar contributes nothing and cannot be referenced anyway -- check 16 sees to that. */
static void gather_slots(const FramedslFrame *frame, size_t packet, size_t count, uint64_t *slots) {
  size_t base = frame->packet_base[packet];
  const FieldInstruction *fields = frame->definition->program->packets[packet].fields;
  for (size_t i = 0; i < count; ++i) {
    const Slot *slot = &frame->slots[base + i];
    /* A fixed field is what the encoder writes, whatever its slot holds -- so it is also what a width
     * or a condition reading it sees, as the decoder will see it on the other end. */
    if (fields[i].has_fixed) {
      slots[i] = fields[i].fixed;
    } else if (slot->is_scalar) {
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

    /* Left out when its conditions do not hold over the values being encoded (#12), whatever its slot
     * holds: a value set on an absent field waits there for the condition. */
    if (instruction->condition_count != 0 && !field_present(instruction, slots)) {
      continue;
    }

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
    } else if (instruction->element != NULL) {
      /* A repetition (#55): as many elements as the count says, straight from the slot. Unlike a bit
       * string handed over whole, its elements are set one member at a time, so there is no length to
       * disagree with -- an element nobody set is zeros, as any field nobody set is. Each element's
       * fixed values come from the definition, as a fixed field's do, whatever the slot holds. */
      write_bytes(frame->output, offset, frame->arena + slot->offset, (size_t)(width / 8));
      const ElementProgram *element = instruction->element;
      for (uint64_t start = 0; start < width; start += element->bits) {
        for (size_t m = 0; m < element->member_count; ++m) {
          const MemberInstruction *member = &element->members[m];
          if (member->has_fixed) {
            frame_member_put(frame->output, offset + start + member->offset_bits, member, member->fixed);
          }
        }
      }
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
