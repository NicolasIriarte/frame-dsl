#include "dispatch.h"

#include <stdlib.h>
#include <string.h>

/* The name index is open addressing over packet indices plus one, zero being empty, at a power-of-two
 * capacity at least twice the packet count: a lookup hashes the name and compares one or two strings.
 *
 * The decode index starts from what every identifiable packet pins. Check 12 has every pair of packets
 * contradict somewhere in their fixed bits; most definitions put that contradiction in a field they all
 * share -- an id, a type, an apid -- so the bits pinned by *every* packet are a key that tells them
 * apart, or nearly. The key is those bits of the first `key_bytes` bytes, no more than eight, which every
 * packet's discriminator covers. Packets are grouped by their value under the key, and a frame's key
 * names its group: the packets left to try in full, in declaration order. Packets told apart only
 * further in share a group, and the full match settles it. When the packets pin no bit in common there
 * is no key, and every packet is a candidate, as before. */

static size_t capacity_for(size_t count) {
  size_t capacity = 8;
  while (capacity < count * 2) {
    capacity *= 2;
  }
  return capacity;
}

typedef struct {
  uint64_t key;
  uint32_t packet;
} Keyed;

static int by_key(const void *a, const void *b) {
  const Keyed *x = a, *y = b;
  if (x->key != y->key) {
    return x->key < y->key ? -1 : 1;
  }
  return (x->packet > y->packet) - (x->packet < y->packet);
}

PacketIndex *packet_index_build(const AstFile *file, const Layout *layout) {
  PacketIndex *index = calloc(1, sizeof(*index));
  if (index == NULL) {
    return NULL;
  }
  const size_t count = file->packet_count;

  index->name_capacity = capacity_for(count);
  index->names = calloc(index->name_capacity, sizeof(*index->names));
  index->order = calloc(count == 0 ? 1 : count, sizeof(*index->order));
  if (index->names == NULL || index->order == NULL) {
    packet_index_free(index);
    return NULL;
  }
  for (size_t p = 0; p < count; ++p) {
    size_t at = (size_t)packet_index_hash(file->packets[p].name) & (index->name_capacity - 1);
    while (index->names[at] != 0) {
      at = (at + 1) & (index->name_capacity - 1);
    }
    index->names[at] = (uint32_t)(p + 1);
  }

  /* The key: the bits every identifiable packet pins, over the bytes all their discriminators cover. */
  size_t key_bytes = 8;
  size_t identifiable = 0;
  for (size_t p = 0; p < count; ++p) {
    const PacketLayout *packet = &layout->packets[p];
    if (packet->discriminator_bits == 0) {
      continue; /* matches nothing, so is never a candidate */
    }
    size_t needed = (packet->discriminator_bits + 7) / 8;
    key_bytes = needed < key_bytes ? needed : key_bytes;
    index->order[identifiable++] = (uint32_t)p;
  }
  index->order_count = identifiable;
  index->key_bytes = identifiable == 0 ? 0 : key_bytes;
  memset(index->key_mask, 0xFF, sizeof(index->key_mask));
  bool any = false;
  for (size_t i = 0; i < index->key_bytes; ++i) {
    for (size_t k = 0; k < identifiable; ++k) {
      index->key_mask[i] &= layout->packets[index->order[k]].mask[i];
    }
    any = any || index->key_mask[i] != 0;
  }
  for (size_t i = 0; i < index->key_bytes; ++i) {
    index->key_mask_word = index->key_mask_word << 8 | index->key_mask[i];
  }
  /* Up to four packets, trying each in turn is as quick as any index: the first matches at once, and
   * the last after three compares of a byte or two. */
  index->few = count <= 4;
  index->keyed = any && identifiable > 4;
  if (!index->keyed) {
    return index; /* in declaration order already */
  }

  Keyed *keyed = malloc(identifiable * sizeof(*keyed));
  index->group_capacity = capacity_for(identifiable);
  index->groups = calloc(index->group_capacity, sizeof(*index->groups));
  if (keyed == NULL || index->groups == NULL) {
    free(keyed);
    packet_index_free(index);
    return NULL;
  }
  for (size_t k = 0; k < identifiable; ++k) {
    keyed[k] =
      (Keyed){ .key = packet_index_key(index, layout->packets[index->order[k]].value), .packet = index->order[k] };
  }
  qsort(keyed, identifiable, sizeof(*keyed), by_key);
  for (size_t k = 0; k < identifiable; ++k) {
    index->order[k] = keyed[k].packet;
  }
  for (size_t k = 0; k < identifiable;) {
    uint64_t key = keyed[k].key;
    size_t end = k + 1;
    while (end < identifiable && keyed[end].key == key) {
      end += 1;
    }
    size_t at = (size_t)packet_index_mix(key) & (index->group_capacity - 1);
    while (index->groups[at].used) {
      at = (at + 1) & (index->group_capacity - 1);
    }
    index->groups[at] = (Group){ .key = key, .start = (uint32_t)k, .count = (uint32_t)(end - k), .used = true };
    k = end;
  }
  free(keyed);
  return index;
}

void packet_index_free(PacketIndex *index) {
  if (index == NULL) {
    return;
  }
  free(index->names);
  free(index->order);
  free(index->groups);
  free(index);
}
