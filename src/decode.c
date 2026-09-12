#include <string.h>

#include "bytecode.h"
#include "definition.h"
#include "frame.h"
#include "framedsl.h"

/* Turning bytes into a frame's values.
 *
 * The inverse of encode.c, and it uses the same bit numbering -- stream bit i is bit 7 - (i % 8) of
 * byte i / 8. Identifying the packet comes first and is not a search through the fields: the layout
 * pass already projected each packet's fixed fields onto the stream bits they pin, as a mask and a
 * value in exactly this numbering, so matching is an AND and a compare per byte. */

static bool read_bit(const uint8_t *buffer, uint64_t index) {
  return ((buffer[index / 8] >> (7 - (index % 8))) & 1) != 0;
}

/* Reads `width` bits at `offset` back into the value they came from, undoing what encode.c laid
 * down. `lsb_first` is not `msb_first` reversed: each puts a different bit of the value at the
 * earliest stream position, so each has to be undone on its own terms. */
static uint64_t read_scalar(const uint8_t *buffer, uint64_t offset, uint64_t width,
                            BitOrder bit_order, ByteOrder byte_order) {
  uint64_t value = 0;

  for (uint64_t j = 0; j < width; ++j) {
    if (!read_bit(buffer, offset + j)) {
      continue;
    }
    value |= bit_order == BIT_ORDER_MSB_FIRST ? ((uint64_t)1 << (width - 1 - j)) : ((uint64_t)1 << j);
  }

  if (byte_order == BYTE_ORDER_LITTLE_ENDIAN && width % 8 == 0 && offset % 8 == 0) {
    uint64_t swapped = 0;
    for (uint64_t byte = 0; byte < width / 8; ++byte) {
      swapped = (swapped << 8) | ((value >> (byte * 8)) & 0xFF);
    }
    value = swapped;
  }
  return value;
}

static void read_bytes(const uint8_t *buffer, uint64_t offset, uint8_t *out, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    uint8_t byte = 0;
    for (uint64_t j = 0; j < 8; ++j) {
      if (read_bit(buffer, offset + i * 8 + j)) {
        byte |= (uint8_t)(1u << (7 - j));
      }
    }
    out[i] = byte;
  }
}

/* Whether this buffer satisfies everything the packet pins.
 *
 * The mask says which stream bits the packet constrains and the value says what it requires them to
 * be, both over the stretch where offsets are still constant. A frame too short to reach the end of
 * that stretch cannot be said to match. */
static bool matches(const PacketLayout *packet, const uint8_t *buffer, size_t length) {
  if (packet->discriminator_bits == 0) {
    return false;
  }
  size_t needed = (packet->discriminator_bits + 7) / 8;
  if (length < needed) {
    return false;
  }

  for (size_t i = 0; i < needed; ++i) {
    if ((buffer[i] & packet->mask[i]) != (packet->value[i] & packet->mask[i])) {
      return false;
    }
  }
  return true;
}

const char *framedsl_decode(FramedslFrame *frame, const uint8_t *buffer, size_t length,
                            size_t *consumed, FramedslStatus *status) {
  FramedslStatus ignored = FRAMEDSL_OK;
  if (status == NULL) {
    status = &ignored;
  }
  *status = FRAMEDSL_OK;

  if (frame == NULL || buffer == NULL) {
    *status = FRAMEDSL_ERROR_MEMORY;
    return NULL;
  }

  /* Set before the walk rather than on each way out, so that every path that gives up below leaves
   * the frame holding nothing. A decode that stops part-way leaves the arena holding however far it
   * got, and the caller finds that out from the next read instead of from a plausible number. */
  frame->holds = HOLDS_NOTHING;
  frame->held_packet = 0;

  const FramedslDefinition *definition = frame->definition;
  const Layout *layout = definition->layout;

  /* Check 12 guarantees at most one packet can match, so the first hit is the answer and there is
   * nothing to disambiguate. */
  size_t index = 0;
  bool found = false;
  for (size_t p = 0; p < layout->packet_count; ++p) {
    if (matches(&layout->packets[p], buffer, length)) {
      index = p;
      found = true;
      break;
    }
  }

  if (!found) {
    /* Nothing on this link looks like anything this definition describes: noise, or a sender running
     * a newer file. Deliberately distinct from a frame that matched and then went wrong, which is a
     * framing bug in the caller's transport and needs a different fix. */
    *status = FRAMEDSL_ERROR_UNRECOGNISED;
    return NULL;
  }

  const PacketProgram *program = &definition->program->packets[index];
  const size_t base = frame->packet_base[index];

  if (program->field_count > FRAMEDSL_MAX_FIELDS_PER_PACKET) {
    *status = FRAMEDSL_ERROR_RANGE;
    return NULL;
  }

  /* Filled in as the walk goes, because a valof() width reads a field this walk has already decoded
   * -- which check 7 guarantees is one it has passed. */
  uint64_t slots[FRAMEDSL_MAX_FIELDS_PER_PACKET] = {0};

  uint64_t offset = 0;
  for (size_t f = 0; f < program->field_count; ++f) {
    const FieldInstruction *instruction = &program->fields[f];
    Slot *slot = &frame->slots[base + f];

    uint64_t width = instruction->width_bits;
    if (instruction->variable) {
      if (!width_program_eval(&instruction->width, slots, &width)) {
        *status = FRAMEDSL_ERROR_RANGE;
        return NULL;
      }
      if (width > instruction->max_bits) {
        /* The frame claims more than the field reserved. This is what the mandatory reservation buys:
         * the slot is never overrun, the walk stops here, and the caller is told -- rather than the
         * library growing a buffer to fit a number a corrupt frame supplied. */
        *status = FRAMEDSL_ERROR_RANGE;
        return NULL;
      }
    }

    /* Checked before any byte of this field is touched, so a frame that runs out is never half-read
     * past the end of the buffer it was given. */
    if (offset + width > (uint64_t)length * 8) {
      *status = FRAMEDSL_ERROR_TRUNCATED;
      return NULL;
    }

    if (instruction->is_scalar) {
      uint64_t value =
          read_scalar(buffer, offset, width, instruction->bit_order, instruction->byte_order);
      slots[f] = value;
      memcpy(frame->arena + slot->offset, &value, sizeof(value));
    } else {
      size_t bytes = (size_t)(width / 8);
      read_bytes(buffer, offset, frame->arena + slot->offset, bytes);
      /* Whatever an earlier frame left past this one's length is cleared, so a shorter payload never
       * shows the tail of a longer one. */
      if (slot->capacity > bytes) {
        memset(frame->arena + slot->offset + bytes, 0, slot->capacity - bytes);
      }
      slot->length = bytes;
    }

    offset += width;
  }

  frame->holds = HOLDS_DECODED_PACKET;
  frame->held_packet = index;

  if (consumed != NULL) {
    *consumed = (size_t)(offset / 8);
  }
  return framedsl_packet_name(definition, index);
}
