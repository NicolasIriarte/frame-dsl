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

struct FramedslFrame {
  const FramedslDefinition *definition;
  uint32_t tag;

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
