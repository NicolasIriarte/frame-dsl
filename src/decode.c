#include <string.h>

#include "bits.h"
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

/* Reads `width` bits at `offset` back into the value they came from, undoing what encode.c laid
 * down. `lsb_first` is not `msb_first` reversed: each puts a different bit of the value at the
 * earliest stream position, so each has to be undone on its own terms. */
static uint64_t read_scalar(
  const uint8_t *buffer,
  uint64_t offset,
  uint64_t width,
  BitOrder bit_order,
  ByteOrder byte_order
) {
  /* little_endian fills each byte from its low bit (#147), the mirror of write_scalar() in encode.c. */
  if (byte_order == BYTE_ORDER_LITTLE_ENDIAN && (width % 8 != 0 || offset % 8 != 0) && width > 0) {
    const uint64_t packed = bits_get_le(buffer, offset, width);
    return bit_order == BIT_ORDER_LSB_FIRST ? bits_reverse(packed, width) : packed;
  }

  /* The stream holds the value's bits in order, so they come in runs: what is left of the current
   * byte, at most eight at a time -- the mirror of write_scalar() in encode.c. lsb_first is the same
   * runs, reversed afterwards (#108). */
  uint64_t value = bits_get(buffer, offset, width);
  if (bit_order == BIT_ORDER_LSB_FIRST && width > 0) {
    value = bits_reverse(value, width);
  }
  if (byte_order == BYTE_ORDER_LITTLE_ENDIAN && width % 8 == 0 && offset % 8 == 0) {
    value = bits_swap(value, width);
  }
  return value;
}

/* The caller has checked the whole field lies inside the buffer. Off a byte boundary that includes
 * the byte after the last whole one, which holds the field's final bits. */
static void read_bytes(const uint8_t *buffer, uint64_t offset, uint8_t *out, size_t length) {
  const uint8_t *at = buffer + offset / 8;
  const unsigned shift = (unsigned)(offset % 8);

  if (shift == 0) {
    if (length > 0) {
      memcpy(out, at, length);
    }
    return;
  }

  /* Eight bytes at a time first (#105), the mirror of write_bytes(): eight buffer bytes as a big-endian
   * word, shifted left by the offset, with the top of the ninth shifted in below. The field reaches
   * buffer byte `length`, so the ninth byte of every step is inside it. Two words a step while there
   * are two, which halves the loop's own cost: each word is only a handful of instructions. */
  size_t i = 0;
  const unsigned back = 8 - shift;
  for (; i + 16 <= length; i += 16) {
    uint64_t first = bits_load_be64(at + i) << shift | (uint64_t)(at[i + 8] >> back);
    uint64_t second = bits_load_be64(at + i + 8) << shift | (uint64_t)(at[i + 16] >> back);
    bits_store_be64(out + i, first);
    bits_store_be64(out + i + 8, second);
  }
  for (; i + 8 <= length; i += 8) {
    bits_store_be64(out + i, bits_load_be64(at + i) << shift | (uint64_t)(at[i + 8] >> back));
  }
  for (; i < length; ++i) {
    out[i] = (uint8_t)((at[i] << shift) | (at[i + 1] >> (8 - shift)));
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

/* The packet a frame is, through the index (#107): only the packets whose shared fixed bits agree with
 * the frame's are matched in full -- usually one. Out of line, because inlined into framedsl_decode() it
 * made every decode of a definition without a key 2-4 ns slower, for code those never run. */
__attribute__((noinline)) static bool find_keyed(
  const FramedslDefinition *definition,
  const uint8_t *buffer,
  size_t length,
  size_t *index
) {
  size_t count = 0;
  const uint32_t *candidates = packet_index_candidates(definition->index, buffer, length, &count);
  for (size_t c = 0; c < count; ++c) {
    if (matches(&definition->layout->packets[candidates[c]], buffer, length)) {
      *index = candidates[c];
      return true;
    }
  }
  return false;
}

/* Reads one frame of packet `index` out of `buffer`, once something has said which packet it is. Inlined
 * into each entry, so that the one every frame goes through pays no call for the other (#146). A walk
 * that gives up leaves `status` saying why, and the frame as it was. */
__attribute__((always_inline)) static inline bool decode_packet(
  FramedslFrame *frame,
  size_t index,
  const uint8_t *buffer,
  size_t length,
  size_t *consumed,
  FramedslStatus *status
) {
  const FramedslDefinition *definition = frame->definition;
  const PacketProgram *program = &definition->program->packets[index];
  const size_t base = frame->packet_base[index];

  if (program->field_count > FRAMEDSL_MAX_FIELDS_PER_PACKET) {
    *status = FRAMEDSL_ERROR_RANGE;
    return false;
  }

  /* The known prefix (#106) fits the frame or it does not, and that is one comparison: past it, each
   * of its fields is read at its place with nothing more to check. */
  const bool prefix_fits = program->prefix_bits <= (uint64_t)length * 8;
  const bool all_prefix = program->prefix_count == program->field_count;

  /* The walk writes each field into its slot as it goes, so a walk that gives up part-way would
   * otherwise leave this packet holding half of one frame and half of another. Put it aside first
   * and put it back on every way out below: a failed decode changes nothing, which is what lets a
   * read be a plain read. Only this packet can be disturbed -- matches() above reads the buffer
   * without writing, and check 12 means no second packet is ever tried. A packet that is all known
   * prefix, in a frame long enough for it, cannot give up part-way, and has nothing to put aside. */
  if (!(all_prefix && prefix_fits)) {
    frame_snapshot_packet(frame, index);
  }

  /* Filled in as the walk goes, because a valof() width or a condition reads a field this walk has
   * already decoded -- which check 7 guarantees is one it has passed, and check 16 a scalar, whose
   * entry is always written before it can be read. */
  uint64_t slots[FRAMEDSL_MAX_FIELDS_PER_PACKET];

  size_t first = 0;
  uint64_t offset = 0;
  if (prefix_fits) {
    for (size_t f = 0; f < program->prefix_count; ++f) {
      const FieldInstruction *instruction = &program->fields[f];
      Slot *slot = &frame->slots[base + f];
      const uint8_t *at = buffer + instruction->offset_bits / 8;
      uint64_t value;
      const unsigned bytes = (unsigned)(instruction->width_bits / 8);
      if (instruction->copy == COPY_BYTES) {
        value = bits_load_be(at, bytes);
      } else if (instruction->copy == COPY_GENERAL) {
        value = read_scalar(
          buffer, instruction->offset_bits, instruction->width_bits, instruction->bit_order, instruction->byte_order
        );
      } else if (instruction->copy == COPY_BYTES_SWAPPED) {
        value = bits_swap(bits_load_be(at, bytes), instruction->width_bits);
      } else if (instruction->copy == COPY_STRING) {
        read_bytes(buffer, instruction->offset_bits, frame->arena + slot->offset, bytes);
        slots[f] = 0;
        continue;
      } else {
        value = bits_load_reversed(at, bytes);
        if (instruction->copy_swap) {
          value = bits_swap(value, instruction->width_bits);
        }
      }
      slots[f] = value;
      memcpy(frame->arena + slot->offset, &value, sizeof(value));
    }
    first = program->prefix_count;
    offset = program->prefix_bits;
  }
  if (all_prefix && prefix_fits) {
    if (consumed != NULL) {
      *consumed = (size_t)(offset / 8);
    }
    return true;
  }

  for (size_t f = first; f < program->field_count; ++f) {
    const FieldInstruction *instruction = &program->fields[f];
    Slot *slot = &frame->slots[base + f];

    /* A field this frame does not carry (#12) takes no bits, and its slot goes back to how a frame
     * starts: a previous frame's value must not come back when a caller later makes the condition hold,
     * and a fixed field reads its fixed value then (#141), and a field with a default its default (#144). */
    if (instruction->condition_count != 0 && !field_present(instruction, slots)) {
      slots[f] = 0;
      memset(frame->arena + slot->offset, 0, slot->capacity);
      frame_seed_field(frame, index, f);
      if (slot->variable) {
        slot->length = 0;
      }
      continue;
    }

    uint64_t width = instruction->width_bits;
    if (instruction->variable) {
      if (!width_program_eval(&instruction->width, slots, &width)) {
        *status = FRAMEDSL_ERROR_RANGE;
        frame_restore_packet(frame, index);
        return false;
      }
      if (width > instruction->max_bits) {
        /* The frame claims more than the field reserved. This is what the mandatory reservation buys:
         * the slot is never overrun, the walk stops here, and the caller is told -- rather than the
         * library growing a buffer to fit a number a corrupt frame supplied. */
        *status = FRAMEDSL_ERROR_RANGE;
        frame_restore_packet(frame, index);
        return false;
      }
    }

    /* Checked before any byte of this field is touched, so a frame that runs out is never half-read
     * past the end of the buffer it was given. */
    if (offset + width > (uint64_t)length * 8) {
      *status = FRAMEDSL_ERROR_TRUNCATED;
      frame_restore_packet(frame, index);
      return false;
    }

    if (instruction->is_scalar) {
      uint64_t value = read_scalar(buffer, offset, width, instruction->bit_order, instruction->byte_order);
      slots[f] = value;
      memcpy(frame->arena + slot->offset, &value, sizeof(value));
    } else {
      size_t bytes = (size_t)(width / 8);
      slots[f] = 0;
      read_bytes(buffer, offset, frame->arena + slot->offset, bytes);
      /* Whatever an earlier frame left past this one's length is cleared, so a shorter payload never
       * shows the tail of a longer one. */
      if (slot->capacity > bytes) {
        memset(frame->arena + slot->offset + bytes, 0, slot->capacity - bytes);
        if (instruction->element != NULL && instruction->element->bits > 0) {
          frame_seed_elements(frame, index, f, (size_t)(width / instruction->element->bits));
        }
      }
      slot->length = bytes;
    }

    offset += width;
  }

  if (consumed != NULL) {
    *consumed = (size_t)(offset / 8);
  }
  return true;
}

const char *framedsl_decode(
  FramedslFrame *frame,
  const uint8_t *buffer,
  size_t length,
  size_t *consumed,
  FramedslStatus *status
) {
  FramedslStatus ignored = FRAMEDSL_OK;
  if (status == NULL) {
    status = &ignored;
  }
  *status = FRAMEDSL_OK;

  if (frame == NULL || buffer == NULL) {
    *status = FRAMEDSL_ERROR_MEMORY;
    return NULL;
  }

  const FramedslDefinition *definition = frame->definition;
  const Layout *layout = definition->layout;

  /* Check 12 guarantees at most one packet can match, so the first hit is the answer and there is
   * nothing to disambiguate. A definition of a few packets, or of packets sharing no fixed bit, tries
   * each in turn; any other goes through the index (#107). */
  size_t index = 0;
  bool found = false;
  if (!definition->index->keyed) {
    for (size_t p = 0; p < layout->packet_count; ++p) {
      if (matches(&layout->packets[p], buffer, length)) {
        index = p;
        found = true;
        break;
      }
    }
  } else {
    found = find_keyed(definition, buffer, length, &index);
  }

  if (!found) {
    /* Nothing on this link looks like anything this definition describes: noise, or a sender running
     * a newer file. Deliberately distinct from a frame that matched and then went wrong, which is a
     * framing bug in the caller's transport and needs a different fix. */
    *status = FRAMEDSL_ERROR_UNRECOGNISED;
    return NULL;
  }

  return decode_packet(frame, index, buffer, length, consumed, status) ? framedsl_packet_name(definition, index) : NULL;
}

FramedslStatus framedsl_decode_as(
  FramedslFrame *frame,
  const char *packet,
  const uint8_t *buffer,
  size_t length,
  size_t *consumed
) {
  if (frame == NULL || buffer == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  size_t index = 0;
  if (packet == NULL || !framedsl_find_packet(frame->definition, packet, &index)
      || !frame->definition->program->packets[index].manual) {
    return FRAMEDSL_ERROR_FIELD;
  }
  FramedslStatus status = FRAMEDSL_OK;
  decode_packet(frame, index, buffer, length, consumed, &status);
  return status;
}
