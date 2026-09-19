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
  /* And the snapshot holds the largest packet's slots, for the same reason: a decode puts the packet
   * it is about to walk aside, and one packet is the most any walk can reach. */
  size_t snapshot_size = 0;
  size_t snapshot_slots = 0;
  for (size_t p = 0; p < layout->packet_count; ++p) {
    const PacketLayout *packet = &layout->packets[p];
    uint64_t packet_bits = 0;
    size_t packet_arena = 0;
    for (size_t f = 0; f < packet->field_count; ++f) {
      const FieldLayout *field = &packet->fields[f];
      arena_size = round_up(arena_size, alignof(uint64_t));
      arena_size += slot_bytes(field);
      /* The same rounding the arena layout below applies, so this is the packet's real footprint
       * there and not an underestimate of it. */
      packet_arena = round_up(packet_arena, alignof(uint64_t)) + slot_bytes(field);
      packet_bits += field->variable ? field->max_bits : field->width_bits;
    }
    size_t packet_bytes = (size_t)((packet_bits + 7) / 8);
    if (packet_bytes > output_size) {
      output_size = packet_bytes;
    }
    if (packet_arena > snapshot_size) {
      snapshot_size = packet_arena;
    }
    if (packet->field_count > snapshot_slots) {
      snapshot_slots = packet->field_count;
    }
  }

  size_t snapshot_offset = round_up(arena_offset + arena_size + output_size, alignof(uint64_t));
  size_t lengths_offset = round_up(snapshot_offset + snapshot_size, alignof(size_t));

  uint8_t *block = calloc(1, lengths_offset + snapshot_slots * sizeof(size_t));
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
  frame->snapshot = block + snapshot_offset;
  frame->snapshot_size = snapshot_size;
  frame->snapshot_lengths = (size_t *)(block + lengths_offset);
  frame->snapshot_slots = snapshot_slots;

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

/* How many slots a packet has, from the base table rather than from the layout: the last packet runs
 * to the end of the slot array, every other one to the next packet's base. */
static size_t packet_field_count(const FramedslFrame *frame, size_t packet) {
  size_t base = frame->packet_base[packet];
  size_t end = packet + 1 < frame->packet_count ? frame->packet_base[packet + 1] : frame->slot_count;
  return end - base;
}

/* The arena range one packet occupies. Its fields are laid out consecutively above, so a packet is a
 * contiguous run and putting it aside is one copy rather than a walk. */
static void packet_extent(const FramedslFrame *frame, size_t packet, size_t *offset, size_t *bytes) {
  size_t base = frame->packet_base[packet];
  size_t count = packet_field_count(frame, packet);

  *offset = 0;
  *bytes = 0;
  if (count == 0) {
    return;
  }

  const Slot *first = &frame->slots[base];
  const Slot *last = &frame->slots[base + count - 1];
  *offset = first->offset;
  *bytes = last->offset + last->capacity - first->offset;
}

void frame_snapshot_packet(FramedslFrame *frame, size_t packet) {
  size_t offset = 0;
  size_t bytes = 0;
  packet_extent(frame, packet, &offset, &bytes);
  if (bytes > frame->snapshot_size) {
    return;
  }

  memcpy(frame->snapshot, frame->arena + offset, bytes);

  size_t base = frame->packet_base[packet];
  size_t count = packet_field_count(frame, packet);
  for (size_t f = 0; f < count && f < frame->snapshot_slots; ++f) {
    frame->snapshot_lengths[f] = frame->slots[base + f].length;
  }
}

void frame_restore_packet(FramedslFrame *frame, size_t packet) {
  size_t offset = 0;
  size_t bytes = 0;
  packet_extent(frame, packet, &offset, &bytes);
  if (bytes > frame->snapshot_size) {
    return;
  }

  memcpy(frame->arena + offset, frame->snapshot, bytes);

  size_t base = frame->packet_base[packet];
  size_t count = packet_field_count(frame, packet);
  for (size_t f = 0; f < count && f < frame->snapshot_slots; ++f) {
    frame->slots[base + f].length = frame->snapshot_lengths[f];
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

bool framedsl_field_id(const FramedslDefinition *definition, size_t packet, size_t field, FramedslFieldId *out) {
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

FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id, const uint8_t *bytes, size_t length) {
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

const uint8_t *framedsl_get_bytes(
  const FramedslFrame *frame,
  FramedslFieldId id,
  size_t *length,
  FramedslStatus *status
) {
  FramedslStatus ignored = FRAMEDSL_OK;
  if (status == NULL) {
    status = &ignored;
  }
  *status = FRAMEDSL_OK;

  if (frame == NULL || length == NULL) {
    *status = FRAMEDSL_ERROR_MEMORY;
    return NULL;
  }

  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar) {
    *status = FRAMEDSL_ERROR_FIELD;
    return NULL;
  }

  /* The bytes are already in their final form in the arena, contiguous and with the slack past
   * `length` zeroed by whoever last wrote the field -- so there is nothing to copy and nothing to
   * size. A field holding nothing gives a usable pointer and a length of zero, because NULL is
   * reserved for "this call failed" and an empty field did not fail. */
  *length = slot->length;
  return frame->arena + slot->offset;
}

FramedslStatus framedsl_get_bytes_into(
  const FramedslFrame *frame,
  FramedslFieldId id,
  uint8_t *out,
  size_t capacity,
  size_t *length
) {
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
