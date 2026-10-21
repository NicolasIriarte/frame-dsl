#include "ast.h"

#include <stdlib.h>
#include <string.h>

AstFile *ast_new(void) {
  AstFile *file = calloc(1, sizeof(AstFile));
  if (file == NULL) {
    return NULL;
  }
  file->region = region_new();
  if (file->region == NULL) {
    free(file);
    return NULL;
  }
  return file;
}

void ast_free(AstFile *file) {
  if (file == NULL) {
    return;
  }
  for (size_t i = 0; i < file->packet_count; ++i) {
    free(file->packets[i].fields);
  }
  free(file->packets);
  for (size_t i = 0; i < file->block_count; ++i) {
    free(file->blocks[i].fields);
  }
  free(file->blocks);
  free(file->cals);
  for (size_t i = 0; i < file->enum_count; ++i) {
    free(file->enums[i].items);
  }
  free(file->enums);
  for (size_t i = 0; i < file->imported_count; ++i) {
    ast_free(file->imported[i]);
  }
  free(file->imported);
  region_free(file->region);
  free(file);
}

static void *grow(void *items, size_t *capacity, size_t count, size_t item_size) {
  if (count < *capacity) {
    return items;
  }
  size_t capacity_next = *capacity == 0 ? 8 : *capacity * 2;
  void *grown = realloc(items, capacity_next * item_size);
  if (grown == NULL) {
    return NULL;
  }
  *capacity = capacity_next;
  return grown;
}

AstPacket *ast_add_packet(AstFile *file) {
  AstPacket *packets = grow(file->packets, &file->packet_capacity, file->packet_count, sizeof(AstPacket));
  if (packets == NULL) {
    return NULL;
  }
  file->packets = packets;

  AstPacket *packet = &file->packets[file->packet_count++];
  memset(packet, 0, sizeof(*packet));
  return packet;
}

AstField *ast_add_field(AstPacket *packet) {
  AstField *fields = grow(packet->fields, &packet->field_capacity, packet->field_count, sizeof(AstField));
  if (fields == NULL) {
    return NULL;
  }
  packet->fields = fields;

  AstField *field = &packet->fields[packet->field_count++];
  memset(field, 0, sizeof(*field));
  return field;
}

AstCal *ast_add_cal(AstFile *file) {
  AstCal *cals = grow(file->cals, &file->cal_capacity, file->cal_count, sizeof(AstCal));
  if (cals == NULL) {
    return NULL;
  }
  file->cals = cals;

  AstCal *cal = &file->cals[file->cal_count++];
  memset(cal, 0, sizeof(*cal));
  return cal;
}

AstPacket *ast_add_block(AstFile *file) {
  AstPacket *blocks = grow(file->blocks, &file->block_capacity, file->block_count, sizeof(AstPacket));
  if (blocks == NULL) {
    return NULL;
  }
  file->blocks = blocks;

  AstPacket *block = &file->blocks[file->block_count++];
  memset(block, 0, sizeof(*block));
  return block;
}

AstEnum *ast_add_enum(AstFile *file) {
  AstEnum *enums = grow(file->enums, &file->enum_capacity, file->enum_count, sizeof(AstEnum));
  if (enums == NULL) {
    return NULL;
  }
  file->enums = enums;

  AstEnum *enumeration = &file->enums[file->enum_count++];
  memset(enumeration, 0, sizeof(*enumeration));
  return enumeration;
}

AstEnumItem *ast_add_enum_item(AstEnum *enumeration) {
  AstEnumItem *items =
    grow(enumeration->items, &enumeration->item_capacity, enumeration->item_count, sizeof(AstEnumItem));
  if (items == NULL) {
    return NULL;
  }
  enumeration->items = items;

  AstEnumItem *item = &enumeration->items[enumeration->item_count++];
  memset(item, 0, sizeof(*item));
  return item;
}

const AstEnum *ast_find_enum(const AstFile *file, const char *name) {
  for (size_t i = 0; file != NULL && name != NULL && i < file->enum_count; ++i) {
    if (file->enums[i].name != NULL && strcmp(file->enums[i].name, name) == 0) {
      return &file->enums[i];
    }
  }
  return NULL;
}

const char *ast_enum_name_of(const AstEnum *enumeration, uint64_t value) {
  size_t low = 0;
  size_t high = enumeration->item_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    uint64_t here = enumeration->items[middle].value;
    if (here == value) {
      return enumeration->items[middle].name;
    }
    if (here < value) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return NULL;
}

const AstEnumItem *ast_enum_item(const AstEnum *enumeration, const char *name) {
  for (size_t i = 0; enumeration != NULL && name != NULL && i < enumeration->item_count; ++i) {
    if (enumeration->items[i].name != NULL && strcmp(enumeration->items[i].name, name) == 0) {
      return &enumeration->items[i];
    }
  }
  return NULL;
}

bool ast_condition_same(const AstCondition *a, const AstCondition *b) {
  if (a == NULL || b == NULL || a->kind != b->kind) {
    return false;
  }
  if (a->kind != CONDITION_COMPARE) {
    return ast_condition_same(a->lhs, b->lhs) && ast_condition_same(a->rhs, b->rhs);
  }
  return a->op == b->op && a->has_mask == b->has_mask && (!a->has_mask || a->mask == b->mask) && a->value == b->value
         && a->field != NULL && b->field != NULL && a->field->reference != NULL && b->field->reference != NULL
         && strcmp(a->field->reference, b->field->reference) == 0;
}

bool ast_conditions_cover(const AstCondition *outer, const AstCondition *inner) {
  for (const AstCondition *needed = inner; needed != NULL; needed = needed->parent) {
    bool found = false;
    for (const AstCondition *held = outer; held != NULL && !found; held = held->parent) {
      found = ast_condition_same(held, needed);
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

bool ast_conditions_equal(const AstCondition *a, const AstCondition *b) {
  return ast_conditions_cover(a, b) && ast_conditions_cover(b, a);
}
