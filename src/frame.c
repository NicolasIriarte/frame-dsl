#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "definition.h"
#include "frame.h"
#include "framedsl.h"

/* The arena, and the accessors that put values into it.
 *
 * One allocation per frame, carved into three regions: the frame header, a slot descriptor per field
 * of every packet, and the bytes those slots point at. One rather than three because the claim the
 * project is built on is that nothing allocates once a frame exists, and a reader checking that
 * claim should find a single malloc to look at.
 *
 * The arena holds host-native values. A scalar sits in it as a uint64_t whatever its declared width;
 * converting to and from the order it has on the wire is the encoder's job, not the caller's. */

static uint32_t next_tag = 1;

uint32_t definition_next_tag(void) {
  return next_tag++;
}

static size_t round_up(size_t value, size_t alignment) {
  size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

/* What one field's slot costs. A scalar is a uint64_t whatever its width, so that reading it is a
 * load rather than a masked assembly of bytes. */
static size_t slot_bytes(const FieldLayout *field) {
  if (field->is_scalar) {
    return sizeof(uint64_t);
  }
  uint64_t bits = field->variable ? field->max_bits : field->width_bits;
  return (size_t)((bits + 7) / 8);
}

FramedslFrame *framedsl_frame_new(const FramedslDefinition *definition) {
  if (definition == NULL) {
    return NULL;
  }

  const Layout *layout = definition->layout;
  size_t slot_count = 0;
  for (size_t p = 0; p < layout->packet_count; ++p) {
    slot_count += layout->packets[p].field_count;
  }

  /* Lay the three regions out before allocating anything, so the whole frame is one block and the
   * no-allocation claim has one call to point at. */
  size_t base_offset = round_up(sizeof(FramedslFrame), alignof(size_t));
  size_t slots_offset = round_up(base_offset + layout->packet_count * sizeof(size_t), alignof(Slot));
  size_t arena_offset = round_up(slots_offset + slot_count * sizeof(Slot), alignof(uint64_t));

  size_t arena_size = 0;
  /* The output buffer holds the largest frame this definition can produce: every field at its full
   * reserved width. Sized once, here, so encoding never asks how big the answer will be. */
  size_t output_size = 0;
  for (size_t p = 0; p < layout->packet_count; ++p) {
    const PacketLayout *packet = &layout->packets[p];
    uint64_t packet_bits = 0;
    for (size_t f = 0; f < packet->field_count; ++f) {
      const FieldLayout *field = &packet->fields[f];
      arena_size = round_up(arena_size, alignof(uint64_t));
      arena_size += slot_bytes(field);
      packet_bits += field->variable ? field->max_bits : field->width_bits;
    }
    size_t packet_bytes = (size_t)((packet_bits + 7) / 8);
    if (packet_bytes > output_size) {
      output_size = packet_bytes;
    }
  }

  uint8_t *block = calloc(1, arena_offset + arena_size + output_size);
  if (block == NULL) {
    return NULL;
  }

  FramedslFrame *frame = (FramedslFrame *)block;
  frame->definition = definition;
  frame->tag = definition->tag;
  frame->packet_base = (size_t *)(block + base_offset);
  frame->packet_count = layout->packet_count;
  frame->slots = (Slot *)(block + slots_offset);
  frame->slot_count = slot_count;
  frame->arena = block + arena_offset;
  frame->arena_size = arena_size;
  frame->output = block + arena_offset + arena_size;
  frame->output_size = output_size;

  size_t slot = 0;
  size_t offset = 0;
  for (size_t p = 0; p < layout->packet_count; ++p) {
    const PacketLayout *packet = &layout->packets[p];
    frame->packet_base[p] = slot;

    for (size_t f = 0; f < packet->field_count; ++f) {
      const FieldLayout *field = &packet->fields[f];
      offset = round_up(offset, alignof(uint64_t));

      frame->slots[slot] = (Slot){
          .offset = offset,
          .capacity = slot_bytes(field),
          /* A fixed-width field is always exactly as long as it is; only a variable one starts
           * empty, and zero is a length it can legitimately have. */
          .length = field->variable ? 0 : slot_bytes(field),
          .width_bits = field->variable ? 0 : field->width_bits,
          .is_scalar = field->is_scalar,
          .variable = field->variable,
      };

      offset += frame->slots[slot].capacity;
      slot += 1;
    }
  }

  return frame;
}

void framedsl_frame_free(FramedslFrame *frame) {
  /* The frame *is* the block, so this frees the slots and the arena with it. */
  free(frame);
}

void framedsl_frame_reset(FramedslFrame *frame) {
  if (frame == NULL) {
    return;
  }
  memset(frame->arena, 0, frame->arena_size);
  for (size_t i = 0; i < frame->slot_count; ++i) {
    frame->slots[i].length = frame->slots[i].variable ? 0 : frame->slots[i].capacity;
  }
}

Slot *frame_slot(const FramedslFrame *frame, FramedslFieldId id) {
  if (frame == NULL || id.owner != frame->tag || id.packet >= frame->packet_count) {
    return NULL;
  }
  const Layout *layout = frame->definition->layout;
  if (id.field >= layout->packets[id.packet].field_count) {
    return NULL;
  }
  return &frame->slots[frame->packet_base[id.packet] + id.field];
}

bool framedsl_field_id(const FramedslDefinition *definition, size_t packet, size_t field,
                       FramedslFieldId *out) {
  if (definition == NULL || out == NULL || packet >= definition->ast->packet_count) {
    return false;
  }
  if (field >= definition->ast->packets[packet].field_count) {
    return false;
  }
  *out = (FramedslFieldId){
      .owner = definition->tag,
      .packet = (uint32_t)packet,
      .field = (uint32_t)field,
  };
  return true;
}

bool framedsl_find(const FramedslDefinition *definition, const char *dotted, FramedslFieldId *out) {
  if (definition == NULL || dotted == NULL || out == NULL) {
    return false;
  }

  /* Split at the *first* dot and look the remainder up whole. Field names cannot contain a dot
   * today, so a two-dot name simply finds nothing -- but when a named block nests one
   * ("PKT.Payload.voltage"), the remainder is what gets split again, and this call does not change
   * shape. */
  const char *dot = strchr(dotted, '.');
  if (dot == NULL || dot == dotted || dot[1] == '\0') {
    return false;
  }
  size_t packet_length = (size_t)(dot - dotted);

  for (size_t p = 0; p < definition->ast->packet_count; ++p) {
    const AstPacket *packet = &definition->ast->packets[p];
    if (strlen(packet->name) != packet_length || memcmp(packet->name, dotted, packet_length) != 0) {
      continue;
    }
    for (size_t f = 0; f < packet->field_count; ++f) {
      if (strcmp(packet->fields[f].name, dot + 1) == 0) {
        return framedsl_field_id(definition, p, f, out);
      }
    }
    return false;
  }
  return false;
}

FramedslStatus framedsl_set_u64(FramedslFrame *frame, FramedslFieldId id, uint64_t value) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  Slot *slot = frame_slot(frame, id);
  if (slot == NULL || !slot->is_scalar) {
    return FRAMEDSL_ERROR_FIELD;
  }

  /* Checked before anything is written, so a rejected value leaves the slot holding what it held. */
  if (slot->width_bits < 64 && value >= ((uint64_t)1 << slot->width_bits)) {
    return FRAMEDSL_ERROR_RANGE;
  }

  memcpy(frame->arena + slot->offset, &value, sizeof(value));
  return FRAMEDSL_OK;
}

FramedslStatus framedsl_get_u64(const FramedslFrame *frame, FramedslFieldId id, uint64_t *out) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || !slot->is_scalar || out == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  memcpy(out, frame->arena + slot->offset, sizeof(*out));
  return FRAMEDSL_OK;
}

FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id, const uint8_t *bytes,
                                  size_t length) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar || (bytes == NULL && length > 0)) {
    return FRAMEDSL_ERROR_FIELD;
  }

  /* A fixed-width field is exactly as long as it is. Taking fewer bytes would leave the rest of it
   * holding whatever was there before, which is a wrong frame rather than a partial one. */
  if (slot->variable ? length > slot->capacity : length != slot->capacity) {
    return FRAMEDSL_ERROR_RANGE;
  }

  if (length > 0) {
    memcpy(frame->arena + slot->offset, bytes, length);
  }
  /* Whatever sat past the new length is cleared, so a shorter value never leaves a longer one's tail
   * visible to the encoder. */
  if (slot->capacity > length) {
    memset(frame->arena + slot->offset + length, 0, slot->capacity - length);
  }
  slot->length = length;
  return FRAMEDSL_OK;
}

FramedslStatus framedsl_get_bytes(const FramedslFrame *frame, FramedslFieldId id, uint8_t *out,
                                  size_t capacity, size_t *length) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar || length == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }

  if (capacity < slot->length || (out == NULL && slot->length > 0)) {
    /* The size needed, so a caller can allocate from the answer rather than from a guess. */
    *length = slot->length;
    return FRAMEDSL_ERROR_BUFFER;
  }

  if (slot->length > 0) {
    memcpy(out, frame->arena + slot->offset, slot->length);
  }
  *length = slot->length;
  return FRAMEDSL_OK;
}

size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id) {
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar) {
    return 0;
  }
  return slot->length;
}
