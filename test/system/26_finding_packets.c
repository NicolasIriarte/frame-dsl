/* Story 26: finding the packet (#107) -- by name for an encode, by its fixed bits for a decode -- without
 * trying every packet in turn, and with the same answer as trying every one would give.
 *
 * The decode side is checked against a reference written here from the public introspection alone: a
 * packet matches a frame when every fixed field at a known place holds its value there, and check 12
 * leaves at most one that does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

/* Records the first failing line and carries on, so a case reads as a list of expectations. */
#define EXPECT(condition)                                                                                              \
  do {                                                                                                                 \
    if (failure == 0 && !(condition)) {                                                                                \
      failure = __LINE__;                                                                                              \
    }                                                                                                                  \
  } while (0)

static uint64_t next(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

static uint64_t bits_at(const uint8_t *frame, uint64_t offset, uint64_t width) {
  uint64_t value = 0;
  for (uint64_t j = 0; j < width; ++j) {
    uint64_t i = offset + j;
    value = value << 1 | ((frame[i / 8] >> (7 - i % 8)) & 1);
  }
  return value;
}

/* The packet the reference says `frame` is, or -1 for none. Only msb_first fixed fields are compared,
 * which is every fixed field these definitions have. */
static long reference_match(const FramedslDefinition *definition, const uint8_t *frame, size_t length) {
  long found = -1;
  for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
    bool matches = true;
    bool any = false;
    for (size_t f = 0; f < framedsl_field_count(definition, p) && matches; ++f) {
      FramedslField field;
      framedsl_field(definition, p, f, &field);
      if (!field.has_fixed || !field.offset_known || field.condition != NULL) {
        continue;
      }
      any = true;
      matches = (field.offset_bits + field.width_bits + 7) / 8 <= length
                && bits_at(frame, field.offset_bits, field.width_bits) == field.fixed;
    }
    if (any && matches) {
      if (found >= 0) {
        return -2; /* two: check 12 should have refused the file */
      }
      found = (long)p;
    }
  }
  return found;
}

/* Random frames, half of them made to match a random packet's fixed fields, decoded and compared with
 * the reference. Lengths run from empty to past the longest packet. */
static int agrees_with_reference(const char *source, unsigned rounds) {
  FramedslResult *result = framedsl_load_string(source, "find.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  int failure = frame == NULL ? __LINE__ : 0;
  uint64_t state = 0x853C49E6748FEA9Bu;
  unsigned matched = 0;
  for (unsigned round = 0; round < rounds && failure == 0; ++round) {
    uint8_t bytes[64];
    size_t length = (size_t)(next(&state) % 24);
    for (size_t i = 0; i < sizeof(bytes); ++i) {
      bytes[i] = (uint8_t)next(&state);
    }
    if (round % 2 == 0) {
      size_t p = (size_t)(next(&state) % framedsl_packet_count(definition));
      for (size_t f = 0; f < framedsl_field_count(definition, p); ++f) {
        FramedslField field;
        framedsl_field(definition, p, f, &field);
        if (field.has_fixed && field.offset_known && field.condition == NULL) {
          for (uint64_t j = 0; j < field.width_bits; ++j) {
            uint64_t i = field.offset_bits + j;
            uint8_t mask = (uint8_t)(1u << (7 - i % 8));
            bool set = ((field.fixed >> (field.width_bits - 1 - j)) & 1) != 0;
            bytes[i / 8] = set ? (uint8_t)(bytes[i / 8] | mask) : (uint8_t)(bytes[i / 8] & ~mask);
          }
        }
      }
    }
    long want = reference_match(definition, bytes, length);
    FramedslStatus status = FRAMEDSL_OK;
    const char *name = framedsl_decode(frame, bytes, length, NULL, &status);
    if (want == -2) {
      failure = __LINE__;
    } else if (want == -1) {
      EXPECT(name == NULL && status == FRAMEDSL_ERROR_UNRECOGNISED);
    } else {
      /* Identified, whether or not the rest of the frame is there. */
      matched += 1;
      EXPECT(status != FRAMEDSL_ERROR_UNRECOGNISED);
      EXPECT(name == NULL || strcmp(name, framedsl_packet_name(definition, (size_t)want)) == 0);
    }
  }
  EXPECT(matched > rounds / 4);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* A family of `count` packets told apart by a 16-bit id, each with two 16-bit fields. */
static char *family(unsigned count) {
  size_t capacity = (size_t)count * 64 + 64;
  char *source = malloc(capacity);
  size_t used = 0;
  for (unsigned p = 0; source != NULL && p < count; ++p) {
    used += (size_t)snprintf(source + used, capacity - used, "pkt P%u { id: 16 = %u; a: 16; b: 16; };\n", p, p + 1);
  }
  return source;
}

/* Every packet of a large family encodes by name and decodes back as itself: the first, the last and
 * every one between. An unknown name, or one differing only in case or by a suffix, is no packet. */
static int us26_tc01_every_packet_of_a_large_family_is_found(const TestArguments *args) {
  (void)args;
  char *source = family(512);
  FramedslResult *result = framedsl_load_string(source, "family.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  int failure = frame == NULL ? __LINE__ : 0;
  for (unsigned p = 0; p < 512 && failure == 0; ++p) {
    char name[16];
    snprintf(name, sizeof(name), "P%u", p);
    size_t index = 0;
    EXPECT(framedsl_find_packet(definition, name, &index) && index == p);
    size_t length = 0;
    const uint8_t *bytes = framedsl_encode(frame, name, &length, NULL);
    EXPECT(bytes != NULL && length == 6 && bytes[0] == (uint8_t)((p + 1) >> 8) && bytes[1] == (uint8_t)(p + 1));
    uint8_t copy[6];
    if (bytes != NULL && length == 6) {
      memcpy(copy, bytes, 6);
      const char *decoded = framedsl_decode(frame, copy, 6, NULL, NULL);
      EXPECT(decoded != NULL && strcmp(decoded, name) == 0);
    }
  }
  FramedslStatus status = FRAMEDSL_OK;
  size_t length = 0;
  EXPECT(!framedsl_find_packet(definition, "P512", NULL) && !framedsl_find_packet(definition, "p0", NULL));
  EXPECT(!framedsl_find_packet(definition, "P0x", NULL) && !framedsl_find_packet(definition, "", NULL));
  EXPECT(framedsl_encode(frame, "P5120", &length, &status) == NULL && status == FRAMEDSL_ERROR_FIELD);
  /* An id no packet has, and a frame too short for any id. */
  const uint8_t stranger[] = { 0x7F, 0xFF, 0, 0, 0, 0 };
  EXPECT(framedsl_decode(frame, stranger, sizeof(stranger), NULL, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_UNRECOGNISED);
  EXPECT(framedsl_decode(frame, stranger, 1, NULL, &status) == NULL && status == FRAMEDSL_ERROR_UNRECOGNISED);
  EXPECT(framedsl_decode(frame, stranger, 0, NULL, &status) == NULL && status == FRAMEDSL_ERROR_UNRECOGNISED);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  free(source);
  return failure;
}

static const char *const SUBTYPES =
  "pkt TC_A { type: 8 = 0x1C; seq: 16; subtype: 8 = 0x40; a: 32; };\n"
  "pkt TC_B { type: 8 = 0x1C; seq: 16; subtype: 8 = 0x41; a: 8; b: 8; };\n"
  "pkt TC_C { type: 8 = 0x1C; seq: 16; subtype: 8 = 0x42; };\n"
  "pkt TM_X { type: 8 = 0x2D; n: 8; };\n";

/* Packets that share their first fixed value and are told apart three bytes in: they share a group,
 * and the full match settles which. */
static int us26_tc02_packets_told_apart_further_in(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(SUBTYPES, "subtypes.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  int failure = frame == NULL ? __LINE__ : 0;
  const struct {
    uint8_t bytes[8];
    size_t length;
    const char *name;
  } frames[] = {
    { { 0x1C, 0x12, 0x34, 0x40, 1, 2, 3, 4 }, 8, "TC_A" },
    { { 0x1C, 0x12, 0x34, 0x41, 5, 6 },       6, "TC_B" },
    { { 0x1C, 0x12, 0x34, 0x42 },             4, "TC_C" },
    { { 0x2D, 0x07 },                         2, "TM_X" },
    { { 0x1C, 0x12, 0x34, 0x43 },             4, NULL   },
    { { 0x1C, 0x12, 0x34 },                   3, NULL   },
  };
  for (size_t i = 0; i < sizeof(frames) / sizeof(*frames) && failure == 0; ++i) {
    FramedslStatus status = FRAMEDSL_OK;
    const char *name = framedsl_decode(frame, frames[i].bytes, frames[i].length, NULL, &status);
    if (frames[i].name == NULL) {
      EXPECT(name == NULL && status == FRAMEDSL_ERROR_UNRECOGNISED);
    } else {
      EXPECT(name != NULL && strcmp(name, frames[i].name) == 0);
    }
  }
  /* One byte is TM_X's type, which is all that identifies it: found, and then short. */
  const uint8_t type_only[] = { 0x2D };
  FramedslStatus status = FRAMEDSL_OK;
  EXPECT(framedsl_decode(frame, type_only, 1, NULL, &status) == NULL && status == FRAMEDSL_ERROR_TRUNCATED);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* Three packets each pair of which contradicts on a different bit, so no bit is pinned by all three:
 * there is nothing to key on, and every packet is tried, as before. */
static const char *const NO_SHARED_BIT =
  "pkt A { x: 1 = 1; y: 1 = 1; rest: 6; };\n"
  "pkt B { x: 1 = 0; pad: 1; z: 1 = 1; rest: 5; };\n"
  "pkt C { pad: 1; y: 1 = 0; z: 1 = 0; rest: 5; };\n";

static int us26_tc03_packets_sharing_no_fixed_bit_are_still_found(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(NO_SHARED_BIT, "unkeyed.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  int failure = frame == NULL ? __LINE__ : 0;
  for (unsigned byte = 0; byte < 256 && failure == 0; ++byte) {
    const uint8_t bytes[] = { (uint8_t)byte };
    long want = reference_match(definition, bytes, 1);
    const char *name = framedsl_decode(frame, bytes, 1, NULL, NULL);
    EXPECT(want != -2);
    EXPECT(want < 0 ? name == NULL : name != NULL && strcmp(name, framedsl_packet_name(definition, (size_t)want)) == 0);
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* Against the reference, over random frames, for families keyed on a leading id, on a field that is
 * not first, on part of a byte, with a group of several, and with no key at all. */
static int us26_tc04_decoding_agrees_with_trying_every_packet(const TestArguments *args) {
  (void)args;
  int failure = 0;
  char *large = family(64);
  const char *const sources[] = {
    large,
    SUBTYPES,
    NO_SHARED_BIT,
    /* All three pin the top nibble; two share its value and differ in the rest of the byte. */
    "pkt A { id: 8 = 0x01; n: 8; };\npkt B { id: 8 = 0x02; n: 16; };\npkt C { k: 4 = 0x3; m: 4; n: 8; };\n",
    /* A CCSDS family: version, type and apid pinned, the apid crossing a byte boundary. */
    "pkt T1 { version: 3 = 0; type: 1 = 0; sec: 1; apid: 11 = 0x101; seq: 16; len: 16; d: 8; };\n"
    "pkt T2 { version: 3 = 0; type: 1 = 0; sec: 1; apid: 11 = 0x102; seq: 16; len: 16; d: 16; };\n"
    "pkt T3 { version: 3 = 0; type: 1 = 1; sec: 1; apid: 11 = 0x101; seq: 16; len: 16; };\n",
  };
  for (size_t s = 0; s < sizeof(sources) / sizeof(*sources) && failure == 0; ++s) {
    failure = agrees_with_reference(sources[s], 20000);
  }
  free(large);
  return failure;
}

void register_user_story26(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us26_tc01_every_packet_of_a_large_family_is_found, "every packet of a large family is found", false);
  ADD_TEST_CASE(us26_tc02_packets_told_apart_further_in, "packets told apart further in are found", false);
  ADD_TEST_CASE(
    us26_tc03_packets_sharing_no_fixed_bit_are_still_found, "packets sharing no fixed bit are still found", false
  );
  ADD_TEST_CASE(us26_tc04_decoding_agrees_with_trying_every_packet, "decoding agrees with trying every packet", false);
}
