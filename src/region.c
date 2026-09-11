#include "region.h"

#include <stdlib.h>
#include <string.h>

#define REGION_BLOCK_MIN (16 * 1024)
#define REGION_ALIGN (sizeof(void *) * 2)

typedef struct Block {
  struct Block *next;
  size_t used;
  size_t size;
  unsigned char data[];
} Block;

struct Region {
  Block *head;
};

static size_t round_up(size_t value, size_t multiple) {
  return (value + multiple - 1) / multiple * multiple;
}

Region *region_new(void) {
  return calloc(1, sizeof(Region));
}

static Block *block_new(size_t need) {
  size_t size = need > REGION_BLOCK_MIN ? need : REGION_BLOCK_MIN;
  Block *block = calloc(1, sizeof(Block) + size);
  if (block == NULL) {
    return NULL;
  }
  block->size = size;
  return block;
}

void *region_alloc(Region *region, size_t size) {
  if (region == NULL) {
    return NULL;
  }

  size = round_up(size == 0 ? 1 : size, REGION_ALIGN);

  if (region->head == NULL || region->head->size - region->head->used < size) {
    Block *block = block_new(size);
    if (block == NULL) {
      return NULL;
    }
    block->next = region->head;
    region->head = block;
  }

  void *result = region->head->data + region->head->used;
  region->head->used += size;
  return result;
}

char *region_strndup(Region *region, const char *text, size_t len) {
  char *copy = region_alloc(region, len + 1);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, text, len);
  copy[len] = '\0';
  return copy;
}

void region_free(Region *region) {
  if (region == NULL) {
    return;
  }
  Block *block = region->head;
  while (block != NULL) {
    Block *next = block->next;
    free(block);
    block = next;
  }
  free(region);
}
