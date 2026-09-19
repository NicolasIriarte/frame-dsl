#pragma once

/**
 * @file
 * @brief The frame's insides, shared with the interpreter.
 *
 * A caller sees an opaque handle; the encoder and the decoder need the arena, the slot table and the
 * output buffer. Kept here rather than reached through accessors, which would be the same coupling
 * written out longer and a function call per field per frame.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "framedsl.h"

typedef struct {
  /** @brief Byte offset into the arena. */
  size_t offset;
  /**
   * @brief Bytes the slot owns: 8 for a scalar, ceil(width/8) for a fixed bit string, and the
   * declared reservation for a variable one.
   */
  size_t capacity;
  /**
   * @brief Bytes actually present. Equal to capacity except for a variable field, where it is
   * whatever the last set_bytes() put there -- so it starts at zero, which is a legitimate length.
   */
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
  /** @brief Index of each packet's first slot, so (packet, field) reaches a slot without a search. */
  size_t *packet_base;
  size_t packet_count;

  uint8_t *arena;
  size_t arena_size;

  /**
   * @brief Where framedsl_encode() builds a frame, sized at creation for the largest packet this
   * definition holds.
   *
   * Handed to the caller as a pointer, valid until the next call on this frame.
   */
  uint8_t *output;
  size_t output_size;

  /**
   * @brief Room to put one packet's slots aside before a decode walks over them, so that a walk
   * which gives up part-way can be undone.
   *
   * Sized at creation for the largest packet, because a decode is told which packet it is reading
   * before it writes anything -- so one packet's worth is all any walk can disturb. Both regions
   * are part of the frame's single allocation.
   */
  uint8_t *snapshot;
  size_t snapshot_size;
  /**
   * @brief The lengths that go with those bytes.
   *
   * A variable field's length lives in its Slot rather than in the arena, so it is saved separately
   * -- or a restored field comes back with the right bytes and the wrong length.
   */
  size_t *snapshot_lengths;
  size_t snapshot_slots;
};

/**
 * @brief The slot an id addresses.
 *
 * The tag check is what stops an id from another definition reading whatever sits at that index.
 *
 * @param frame The frame to look in.
 * @param id A field id.
 * @return The slot, or NULL if `id` does not address one of this frame's fields.
 */
Slot *frame_slot(const FramedslFrame *frame, FramedslFieldId id);

/**
 * @brief Puts one packet's slots aside, so that a decode over them can be undone.
 *
 * Called by the decoder before the walk's first write. Neither this nor frame_restore_packet()
 * allocates: the room is part of the frame, sized at creation for the largest packet.
 *
 * @param frame The frame whose arena is about to be written.
 * @param packet Index of the packet the decoder is about to walk.
 */
void frame_snapshot_packet(FramedslFrame *frame, size_t packet);

/**
 * @brief Puts back what frame_snapshot_packet() set aside.
 *
 * Called on every way out of a walk that gave up part-way, so a failed decode leaves every field
 * holding what it held before -- which is what lets a read be a plain read with nothing to check.
 *
 * @param frame The frame to put back.
 * @param packet Index of the packet that was snapshotted.
 */
void frame_restore_packet(FramedslFrame *frame, size_t packet);
