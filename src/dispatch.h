#pragma once

/**
 * @file
 * @brief Finding a packet without trying each in turn (#107): by name for an encode, by the bits a frame
 * starts with for a decode. Both indexes are built once, when the definition loads.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ast.h"
#include "bits.h"
#include "check.h"

/** @brief The packets sharing one value under the key: `count` of them in the index's order, from
 * `start`. */
typedef struct {
  uint64_t key;
  uint32_t start;
  uint32_t count;
  bool used;
} Group;

/** @brief Both indexes; how they are built is in dispatch.c. Here so the lookups can be inlined into
 * the encoder and the decoder, which call them once per frame. */
typedef struct PacketIndex {
  uint32_t *names;
  size_t name_capacity;

  size_t key_bytes;
  uint8_t key_mask[8];
  /** @brief key_mask as the big-endian number its bytes spell, for a key read in one load. */
  uint64_t key_mask_word;
  bool keyed;
  /** @brief Few enough packets that comparing names in turn costs no more than hashing one. */
  bool few;
  /** @brief Every identifiable packet, ordered by key and then by declaration; when unkeyed, just by
   * declaration. */
  uint32_t *order;
  size_t order_count;
  Group *groups;
  size_t group_capacity;
} PacketIndex;

/**
 * @brief Builds both indexes for a checked file.
 * @return The index, or NULL when out of memory.
 */
PacketIndex *packet_index_build(const AstFile *file, const Layout *layout);

void packet_index_free(PacketIndex *index);

static inline uint64_t packet_index_hash(const char *name) {
  uint64_t hash = UINT64_C(0xcbf29ce484222325);
  for (const unsigned char *c = (const unsigned char *)name; *c != '\0'; ++c) {
    hash = (hash ^ *c) * UINT64_C(0x100000001b3);
  }
  return hash;
}

static inline uint64_t packet_index_mix(uint64_t key) {
  key ^= key >> 33;
  key *= UINT64_C(0xff51afd7ed558ccd);
  key ^= key >> 33;
  return key;
}

/* A frame's (or a packet's) bits under the key, as one number. */
static inline uint64_t packet_index_key(const PacketIndex *index, const uint8_t *bytes) {
  return bits_load_be(bytes, (unsigned)index->key_bytes) & index->key_mask_word;
}

/** @brief The packet called `name`, as framedsl_find_packet() answers it. */
static inline bool packet_index_find(const PacketIndex *index, const AstFile *file, const char *name, size_t *packet) {
  if (index->few) {
    for (size_t p = 0; p < file->packet_count; ++p) {
      if (strcmp(file->packets[p].name, name) == 0) {
        if (packet != NULL) {
          *packet = p;
        }
        return true;
      }
    }
    return false;
  }
  size_t at = (size_t)packet_index_hash(name) & (index->name_capacity - 1);
  for (uint32_t entry = index->names[at]; entry != 0; entry = index->names[at]) {
    if (strcmp(file->packets[entry - 1].name, name) == 0) {
      if (packet != NULL) {
        *packet = entry - 1;
      }
      return true;
    }
    at = (at + 1) & (index->name_capacity - 1);
  }
  return false;
}

/**
 * @brief The packets a frame starting with `buffer` could be, in declaration order: every one whose
 * fixed bits shared by all packets agree with the frame's. The caller still matches each in full.
 * @param count Set to how many there are; zero when the frame is too short for any packet.
 * @return The packets' indices, valid as long as the index.
 */
static inline const uint32_t *packet_index_candidates(
  const PacketIndex *index,
  const uint8_t *buffer,
  size_t length,
  size_t *count
) {
  if (!index->keyed) {
    *count = index->order_count;
    return index->order;
  }
  /* Every packet's discriminator covers the key's bytes, so a frame shorter than that matches none. */
  if (length < index->key_bytes) {
    *count = 0;
    return index->order;
  }
  const uint64_t key = packet_index_key(index, buffer);
  size_t at = (size_t)packet_index_mix(key) & (index->group_capacity - 1);
  for (; index->groups[at].used; at = (at + 1) & (index->group_capacity - 1)) {
    if (index->groups[at].key == key) {
      *count = index->groups[at].count;
      return index->order + index->groups[at].start;
    }
  }
  *count = 0;
  return index->order;
}
