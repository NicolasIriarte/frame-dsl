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
  free(file->cals);
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
