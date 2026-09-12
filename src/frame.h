#pragma once

/* The frame's insides, shared with the interpreter.
 *
 * A caller sees an opaque handle; the encoder and the decoder need the arena, the slot table and the
 * output buffer. Kept here rather than reached through accessors, which would be the same coupling
 * written out longer and a function call per field per frame. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "framedsl.h"

typedef struct {
  /* Byte offset into the arena. */
  size_t offset;
  /* Bytes the slot owns: 8 for a scalar, ceil(width/8) for a fixed bit string, and the declared
   * reservation for a variable one. */
  size_t capacity;
  /* Bytes actually present. Equal to capacity except for a variable field, where it is whatever the
   * last set_bytes() put there -- so it starts at zero, which is a legitimate length. */
  size_t length;

  uint64_t width_bits;
  bool is_scalar;
  bool variable;
} Slot;

/* What the frame is currently holding, which decides what may be read out of it. The arena has a slot
 * per field of every packet, so without this a decoded frame answers for packets that did not
 * arrive. */
typedef enum {
  /* Whatever a caller put there: a new frame, a reset one, or one that has been written to. */
  HOLDS_CALLER_VALUES,
  /* One decoded packet, named by `held_packet`. */
  HOLDS_DECODED_PACKET,
  /* Nothing readable -- a decode stopped part-way and the arena is however far it got. */
  HOLDS_NOTHING,
} FrameContents;

struct FramedslFrame {
  const FramedslDefinition *definition;
  uint32_t tag;

  FrameContents holds;
  /* Meaningful only while `holds` is HOLDS_DECODED_PACKET. */
  size_t held_packet;

  Slot *slots;
  size_t slot_count;
  /* Index of each packet's first slot, so (packet, field) reaches a slot without a search. */
  size_t *packet_base;
  size_t packet_count;

  uint8_t *arena;
  size_t arena_size;

  /* Where framedsl_encode() builds a frame, sized at creation for the largest packet this definition
   * holds. Handed to the caller as a pointer, valid until the next call on this frame. */
  uint8_t *output;
  size_t output_size;
};

/* The slot an id addresses, or NULL if it does not address one of this frame's fields. The tag check
 * is what stops an id from another definition reading whatever sits at that index. */
Slot *frame_slot(const FramedslFrame *frame, FramedslFieldId id);

/* Whether the frame is currently holding the packet this id belongs to. A write is not asked -- a
 * caller writing a value is taking the frame back, which frame_taken_by_caller() records. */
bool frame_holds(const FramedslFrame *frame, FramedslFieldId id);
void frame_taken_by_caller(FramedslFrame *frame);
