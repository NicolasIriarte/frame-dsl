#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
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

/* What a frame holds before anything is written to it: zeros, which the arena already is, except that a
 * fixed field holds its fixed value (#140) and a field with a default holds that (#144), a repetition's
 * members included. A frame being built then never reads back a value no frame of it could carry, and a
 * decode replaces these with what arrived. */
void frame_seed_field(FramedslFrame *frame, size_t packet, size_t field) {
  const FieldInstruction *instruction = &frame->definition->program->packets[packet].fields[field];
  uint8_t *at = frame->arena + frame->slots[frame->packet_base[packet] + field].offset;
  if (instruction->has_fixed) {
    memcpy(at, &instruction->fixed, sizeof(instruction->fixed));
  } else if (instruction->has_default) {
    memcpy(at, &instruction->default_value, sizeof(instruction->default_value));
  } else if (instruction->element != NULL) {
    stamp_fixed(at, 0, instruction->element_capacity * instruction->element->bits, instruction->element);
    frame_seed_elements(frame, packet, field, 0);
  }
}

/* Unlike a fixed value, a default is not stamped on encode, where it would overwrite what a program set:
 * this runs only where a frame's elements start out. */
void frame_seed_elements(FramedslFrame *frame, size_t packet, size_t field, size_t first) {
  const FieldInstruction *instruction = &frame->definition->program->packets[packet].fields[field];
  const ElementProgram *element = instruction->element;
  if (element->default_members == 0) {
    return;
  }
  uint8_t *at = frame->arena + frame->slots[frame->packet_base[packet] + field].offset;
  for (size_t i = first; i < instruction->element_capacity; ++i) {
    for (size_t m = 0; m < element->member_count; ++m) {
      const MemberInstruction *member = &element->members[m];
      if (member->has_default) {
        member_put(at, i * element->bits + member->offset_bits, member, member->default_value);
      }
    }
  }
}

static void frame_seed(FramedslFrame *frame) {
  for (size_t p = 0; p < frame->packet_count; ++p) {
    for (size_t f = 0; f < frame->definition->program->packets[p].field_count; ++f) {
      frame_seed_field(frame, p, f);
    }
  }
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
        .is_fixed = definition->program->packets[p].fields[f].has_fixed,
      };

      offset += frame->slots[slot].capacity;
      slot += 1;
    }
  }

  frame_seed(frame);
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
  frame_seed(frame);
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
    .packet = (uint16_t)packet,
    .field = (uint16_t)field,
    .element = FRAMEDSL_NO_ELEMENT,
  };
  return true;
}

static bool find_member(
  const FramedslDefinition *definition,
  size_t packet,
  size_t field,
  const char *name,
  FramedslFieldId *out
);

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
    for (size_t f = 0; f < packet->field_count; ++f) {
      if (packet->fields[f].element != NULL && find_member(definition, p, f, dot + 1, out)) {
        return true;
      }
    }
    return false;
  }
  return false;
}

/* ---- Presence (#12) ------------------------------------------------------------------------------
 *
 * A conditional field is present when its conditions hold over the values the frame holds now --
 * decoded or set, alike. Nothing is stored for it: a condition reads the slots of the fields it names,
 * which are scalars (check 16), so this is a load and a compare per condition. A member of a repetition
 * is present when the repetition is. */

static bool field_absent(const FramedslFrame *frame, FramedslFieldId id) {
  const FieldInstruction *instruction = &frame->definition->program->packets[id.packet].fields[id.field];
  const size_t base = frame->packet_base[id.packet];
  /* field_present()'s walk, reading each value where it is rather than from a gathered array. */
  uint32_t at = instruction->condition_entry;
  while (at < instruction->condition_count) {
    const ConditionInstruction *condition = &instruction->conditions[at];
    /* What the frame holds: a fixed field's fixed value until a decode replaces it with what arrived
     * (#141), so this answers for a frame being built and a decoded one alike. */
    uint64_t value;
    memcpy(&value, frame->arena + frame->slots[base + condition->field].offset, sizeof(value));
    at = condition_test(condition, value) ? condition->if_true : condition->if_false;
  }
  return at == CONDITION_FALSE;
}

FramedslStatus framedsl_present(const FramedslFrame *frame, FramedslFieldId id, bool *present) {
  if (frame == NULL || present == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  if (frame_slot(frame, id) == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  *present = !field_absent(frame, id);
  return FRAMEDSL_OK;
}

/* ---- Repetitions (#55) ----------------------------------------------------------------------------
 *
 * A repetition is one slot holding its elements as they are on the wire, one after another. A member of
 * element i is `i * element bits + its offset in the element` into those bytes, and is read and written
 * there, in its own bit and byte order -- so there is no second copy of anything to keep in step, and
 * encode and decode move the whole run as the bit string it is. */

/* The most elements a repetition has room for: what it reserves, in whole elements. Worked out when
 * the program is compiled (#101), since it is the definition's and not the frame's. */
static uint64_t element_capacity(const FramedslDefinition *definition, size_t packet, size_t field) {
  return definition->program->packets[packet].fields[field].element_capacity;
}

/* "samples[3].value" or "samples.value" against repetition `field`: the member id, or false. */
static bool find_member(
  const FramedslDefinition *definition,
  size_t packet,
  size_t field,
  const char *name,
  FramedslFieldId *out
) {
  const char *repetition = definition->ast->packets[packet].fields[field].name;
  size_t length = strlen(repetition);
  if (strncmp(name, repetition, length) != 0) {
    return false;
  }
  const char *at = name + length;

  uint64_t index = FRAMEDSL_NO_ELEMENT;
  if (*at == '[') {
    at += 1;
    if (*at < '0' || *at > '9') {
      return false;
    }
    index = 0;
    while (*at >= '0' && *at <= '9') {
      index = index * 10 + (uint64_t)(*at - '0');
      if (index >= FRAMEDSL_NO_ELEMENT) {
        return false;
      }
      at += 1;
    }
    if (*at != ']') {
      return false;
    }
    at += 1;
    if (index >= element_capacity(definition, packet, field)) {
      return false;
    }
  }
  if (*at != '.') {
    return false;
  }

  const ElementProgram *element = definition->program->packets[packet].fields[field].element;
  for (size_t m = 0; m < element->member_count; ++m) {
    if (strcmp(element->members[m].name, at + 1) == 0) {
      framedsl_field_id(definition, packet, field, out);
      out->member = (uint16_t)(m + 1);
      out->element = (uint32_t)index;
      return true;
    }
  }
  return false;
}

FramedslStatus framedsl_element(
  const FramedslDefinition *definition,
  FramedslFieldId id,
  size_t index,
  FramedslFieldId *out
) {
  if (definition == NULL || out == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  if (id.owner != definition->tag || id.member == 0 || id.packet >= definition->program->packet_count
      || id.field >= definition->program->packets[id.packet].field_count) {
    return FRAMEDSL_ERROR_FIELD;
  }
  const ElementProgram *element = definition->program->packets[id.packet].fields[id.field].element;
  if (element == NULL || id.member > element->member_count) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (index >= element_capacity(definition, id.packet, id.field)) {
    return FRAMEDSL_ERROR_RANGE;
  }
  *out = id;
  out->element = (uint32_t)index;
  return FRAMEDSL_OK;
}

FramedslStatus framedsl_count(const FramedslFrame *frame, FramedslFieldId id, size_t *count) {
  if (frame == NULL || count == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  if (frame_slot(frame, id) == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  const PacketProgram *packet = &frame->definition->program->packets[id.packet];
  const FieldInstruction *instruction = &packet->fields[id.field];
  if (instruction->element == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (field_absent(frame, id)) {
    return FRAMEDSL_ERROR_ABSENT;
  }

  uint64_t width = instruction->width_bits;
  if (instruction->variable) {
    /* The count field's value now, whoever put it there: the same arithmetic encode will do. */
    uint64_t values[FRAMEDSL_MAX_FIELDS_PER_PACKET];
    if (packet->field_count > FRAMEDSL_MAX_FIELDS_PER_PACKET) {
      return FRAMEDSL_ERROR_RANGE;
    }
    const size_t base = frame->packet_base[id.packet];
    for (size_t f = 0; f < packet->field_count; ++f) {
      const Slot *slot = &frame->slots[base + f];
      values[f] = 0;
      if (slot->is_scalar) {
        memcpy(&values[f], frame->arena + slot->offset, sizeof(uint64_t));
      }
    }
    if (!width_program_eval(&instruction->width, values, &width) || width > instruction->max_bits) {
      return FRAMEDSL_ERROR_RANGE;
    }
  }
  *count = (size_t)(width / instruction->element->bits);
  return FRAMEDSL_OK;
}

/* A member of one element, where it sits in its repetition's slot. */
typedef struct {
  Slot *slot;
  /** @brief The repetition's. */
  const FieldInstruction *instruction;
  const MemberInstruction *member;
  uint64_t offset;
} MemberView;

static inline bool member_view(const FramedslFrame *frame, FramedslFieldId id, MemberView *view) {
  /* frame_slot()'s checks, made against the program, whose field instruction is what the rest needs:
   * one walk from the definition down rather than one per question (#101). A packet's program has as
   * many fields as its layout. */
  if (frame == NULL || id.owner != frame->tag || id.packet >= frame->packet_count || id.member == 0
      || id.element == FRAMEDSL_NO_ELEMENT) {
    return false;
  }
  const PacketProgram *packet = &frame->definition->program->packets[id.packet];
  if (id.field >= packet->field_count) {
    return false;
  }
  const FieldInstruction *instruction = &packet->fields[id.field];
  const ElementProgram *element = instruction->element;
  if (element == NULL || id.member > element->member_count || id.element >= instruction->element_capacity) {
    return false;
  }
  view->slot = &frame->slots[frame->packet_base[id.packet] + id.field];
  view->instruction = instruction;
  view->member = &element->members[id.member - 1];
  view->offset = (uint64_t)id.element * element->bits + view->member->offset_bits;
  return true;
}

/* The bytes a whole-byte member occupies, or NULL for one that does not start and end on a byte. */
static uint8_t *member_bytes(const FramedslFrame *frame, const MemberView *view) {
  if (view->offset % 8 != 0 || view->member->width_bits % 8 != 0) {
    return NULL;
  }
  return frame->arena + view->slot->offset + view->offset / 8;
}

__attribute__((noinline)) static FramedslStatus member_set_u64(
  FramedslFrame *frame,
  FramedslFieldId id,
  uint64_t value
) {
  MemberView view;
  if (!member_view(frame, id, &view) || !view.member->is_scalar) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (view.member->has_fixed) {
    return FRAMEDSL_ERROR_FIXED;
  }
  if (view.member->width_bits < 64 && value >= ((uint64_t)1 << view.member->width_bits)) {
    return FRAMEDSL_ERROR_RANGE;
  }
  member_put(frame->arena + view.slot->offset, view.offset, view.member, value);
  return FRAMEDSL_OK;
}

/* Out of line, so that inlining the member path does not cost an ordinary field's get its lean entry. */
__attribute__((noinline)) static FramedslStatus member_get_u64(
  const FramedslFrame *frame,
  FramedslFieldId id,
  uint64_t *out
) {
  MemberView view;
  if (!member_view(frame, id, &view) || !view.member->is_scalar || out == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (view.instruction->condition_count != 0 && field_absent(frame, id)) {
    return FRAMEDSL_ERROR_ABSENT;
  }
  *out = member_get(frame->arena + view.slot->offset, view.offset, view.member);
  return FRAMEDSL_OK;
}

FramedslStatus framedsl_set_u64(FramedslFrame *frame, FramedslFieldId id, uint64_t value) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  if (id.member != 0) {
    return member_set_u64(frame, id, value);
  }
  Slot *slot = frame_slot(frame, id);
  if (slot == NULL || !slot->is_scalar) {
    return FRAMEDSL_ERROR_FIELD;
  }
  /* The encoder writes the definition's value whatever a slot holds, so accepting this would report a
   * write that never reaches the wire. */
  if (slot->is_fixed) {
    return FRAMEDSL_ERROR_FIXED;
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
  if (id.member != 0) {
    return member_get_u64(frame, id, out);
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || !slot->is_scalar || out == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (field_absent(frame, id)) {
    return FRAMEDSL_ERROR_ABSENT;
  }
  memcpy(out, frame->arena + slot->offset, sizeof(*out));
  return FRAMEDSL_OK;
}

/* ---- Engineering values -----------------------------------------------------------------------
 *
 * The arena holds raw values only; a curve runs on the way out of a get and on the way into a set. */

static const CalCompiled *field_cal(const FramedslFrame *frame, FramedslFieldId id) {
  const FieldInstruction *instruction = &frame->definition->program->packets[id.packet].fields[id.field];
  int32_t cal = instruction->cal;
  if (id.member != 0) {
    MemberView view;
    cal = member_view(frame, id, &view) ? view.member->cal : -1;
  }
  return cal < 0 ? NULL : &frame->definition->cals->cals[cal];
}

/* Whether the field or member `id` has a fixed value. Only asked once the id has been found to address
 * one, and before any question about the value, so that a setter's answer is the same whatever it was
 * given (#140). A member with no element chosen is not one, and set_u64() refuses it. */
static bool field_fixed(const FramedslFrame *frame, FramedslFieldId id) {
  if (id.member == 0) {
    return frame_slot(frame, id)->is_fixed;
  }
  MemberView view;
  return member_view(frame, id, &view) && view.member->has_fixed;
}

FramedslStatus framedsl_get_eng(const FramedslFrame *frame, FramedslFieldId id, double *out) {
  uint64_t raw = 0;
  FramedslStatus status = framedsl_get_u64(frame, id, &raw);
  if (status != FRAMEDSL_OK) {
    return status;
  }
  const CalCompiled *cal = field_cal(frame, id);
  if (cal == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  return cal_forward(cal, (double)raw, out) ? FRAMEDSL_OK : FRAMEDSL_ERROR_RANGE;
}

/* To the nearest integer, half away from zero, refusing what no unsigned field can hold. Written out
 * rather than taken from libm: `x + 0.5` truncated is wrong just below a half, and this is short. */
static bool nearest_raw(double x, uint64_t *out) {
  if (!(x >= -0.5) || !(x < 18446744073709551615.5)) {
    return false; /* also catches NaN, which fails every comparison */
  }
  if (x < 0) {
    *out = 0;
    return true;
  }
  uint64_t whole = (uint64_t)x;
  if (x - (double)whole >= 0.5) {
    if (whole == UINT64_MAX) {
      return false;
    }
    whole += 1;
  }
  *out = whole;
  return true;
}

FramedslStatus framedsl_set_eng(FramedslFrame *frame, FramedslFieldId id, double value) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || (id.member == 0 && !slot->is_scalar)) {
    return FRAMEDSL_ERROR_FIELD;
  }
  const CalCompiled *cal = field_cal(frame, id);
  if (cal == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (field_fixed(frame, id)) {
    return FRAMEDSL_ERROR_FIXED;
  }
  if (!cal->invertible) {
    return FRAMEDSL_ERROR_FIELD;
  }

  double raw = 0;
  uint64_t rounded = 0;
  if (!cal_backward(cal, value, &raw) || !nearest_raw(raw, &rounded)) {
    return FRAMEDSL_ERROR_RANGE;
  }
  /* The width check, and the write, are set_u64()'s. */
  return framedsl_set_u64(frame, id, rounded);
}

FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id, const uint8_t *bytes, size_t length) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  if (id.member != 0) {
    /* A whole-byte bit string inside an element: exactly its width, nothing to clear past it. */
    MemberView view;
    uint8_t *at = member_view(frame, id, &view) && !view.member->is_scalar ? member_bytes(frame, &view) : NULL;
    if (at == NULL || (bytes == NULL && length > 0)) {
      return FRAMEDSL_ERROR_FIELD;
    }
    if (length != view.member->width_bits / 8) {
      return FRAMEDSL_ERROR_RANGE;
    }
    if (length > 0) {
      memcpy(at, bytes, length);
    }
    return FRAMEDSL_OK;
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

  if (id.member != 0) {
    MemberView view;
    uint8_t *at = member_view(frame, id, &view) && !view.member->is_scalar ? member_bytes(frame, &view) : NULL;
    if (at == NULL) {
      *status = FRAMEDSL_ERROR_FIELD;
      return NULL;
    }
    if (field_absent(frame, id)) {
      *status = FRAMEDSL_ERROR_ABSENT;
      return NULL;
    }
    *length = (size_t)(view.member->width_bits / 8);
    return at;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar) {
    *status = FRAMEDSL_ERROR_FIELD;
    return NULL;
  }
  if (field_absent(frame, id)) {
    *status = FRAMEDSL_ERROR_ABSENT;
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
  if (id.member != 0) {
    FramedslStatus status = FRAMEDSL_OK;
    size_t size = 0;
    const uint8_t *at = length != NULL ? framedsl_get_bytes(frame, id, &size, &status) : NULL;
    if (at == NULL) {
      return status == FRAMEDSL_ERROR_ABSENT ? status : FRAMEDSL_ERROR_FIELD;
    }
    *length = size;
    if (capacity < size || (out == NULL && size > 0)) {
      return FRAMEDSL_ERROR_BUFFER;
    }
    if (size > 0) {
      memcpy(out, at, size);
    }
    return FRAMEDSL_OK;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar || length == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (field_absent(frame, id)) {
    return FRAMEDSL_ERROR_ABSENT;
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
  if (id.member != 0) {
    size_t length = 0;
    FramedslStatus status = FRAMEDSL_OK;
    return framedsl_get_bytes(frame, id, &length, &status) != NULL ? length : 0;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || slot->is_scalar || field_absent(frame, id)) {
    return 0;
  }
  return slot->length;
}

/* ---- Names (#11) ------------------------------------------------------------------------------
 *
 * The arena holds the number, as for any field. A name is looked up on the way out of a get and on the
 * way into a set, the way an engineering value is. */

/* The enum a field is typed by, or NULL. Only asked once the id has been found to address a field. */
static const AstEnum *field_enum(const FramedslFrame *frame, FramedslFieldId id) {
  int32_t enumeration = frame->definition->program->packets[id.packet].fields[id.field].enumeration;
  if (id.member != 0) {
    MemberView view;
    enumeration = member_view(frame, id, &view) ? view.member->enumeration : -1;
  }
  return enumeration < 0 ? NULL : &frame->definition->ast->enums[enumeration];
}

FramedslStatus framedsl_get_name(const FramedslFrame *frame, FramedslFieldId id, const char **name) {
  uint64_t value = 0;
  FramedslStatus status = framedsl_get_u64(frame, id, &value);
  if (status != FRAMEDSL_OK) {
    return status;
  }
  const AstEnum *enumeration = field_enum(frame, id);
  if (enumeration == NULL || name == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  *name = ast_enum_name_of(enumeration, value);
  return *name != NULL ? FRAMEDSL_OK : FRAMEDSL_ERROR_UNLISTED;
}

FramedslStatus framedsl_set_name(FramedslFrame *frame, FramedslFieldId id, const char *name) {
  if (frame == NULL) {
    return FRAMEDSL_ERROR_MEMORY;
  }
  const Slot *slot = frame_slot(frame, id);
  if (slot == NULL || (id.member == 0 && !slot->is_scalar)) {
    return FRAMEDSL_ERROR_FIELD;
  }
  const AstEnum *enumeration = field_enum(frame, id);
  if (enumeration == NULL) {
    return FRAMEDSL_ERROR_FIELD;
  }
  if (field_fixed(frame, id)) {
    return FRAMEDSL_ERROR_FIXED;
  }
  const AstEnumItem *item = ast_enum_item(enumeration, name);
  if (item == NULL) {
    return FRAMEDSL_ERROR_UNLISTED;
  }
  /* The width check, and the write, are set_u64()'s; the enum's values all fit its width (check 38). */
  return framedsl_set_u64(frame, id, item->value);
}
