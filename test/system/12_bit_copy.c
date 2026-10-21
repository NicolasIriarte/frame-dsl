/* Story 12: fields at every bit offset, against a reference.
 *
 * Encoding and decoding move whole runs of bits at once -- a copy on a byte boundary, shifted bytes off
 * one, and scalars up to eight bits at a time (#62). That is faster and easy to get wrong at exactly
 * one offset, so this story does not trust a hand-picked case: it puts each kind of field at each of
 * the eight bit offsets, encodes random values, and compares the bytes with a plain bit-at-a-time
 * reference written here, then decodes them back. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

/* Stream bit i is bit 7 - (i % 8) of byte i / 8 -- the numbering in docs/design.md, and all the
 * reference needs to know. */
static void reference_bit(uint8_t *out, uint64_t index, bool set) {
  if (set) {
    out[index / 8] |= (uint8_t)(1u << (7 - (index % 8)));
  }
}

static void reference_scalar(uint8_t *out, uint64_t offset, uint64_t width, uint64_t value) {
  for (uint64_t j = 0; j < width; ++j) {
    reference_bit(out, offset + j, ((value >> (width - 1 - j)) & 1) != 0);
  }
}

/* lsb_first puts the value's bit j at the field's stream bit j. */
static void reference_scalar_lsb_first(uint8_t *out, uint64_t offset, uint64_t width, uint64_t value) {
  for (uint64_t j = 0; j < width; ++j) {
    reference_bit(out, offset + j, ((value >> j) & 1) != 0);
  }
}

static void reference_bytes(uint8_t *out, uint64_t offset, const uint8_t *bytes, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    for (uint64_t j = 0; j < 8; ++j) {
      reference_bit(out, offset + i * 8 + j, ((bytes[i] >> (7 - j)) & 1) != 0);
    }
  }
}

/* A deterministic generator, so a failure reproduces. */
static uint64_t next(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

static uint64_t masked(uint64_t value, uint64_t width) {
  return width >= 64 ? value : value & ((UINT64_C(1) << width) - 1);
}

/* One packet: an 8-bit id, `lead` bits of padding to set the offset, the field under test, and
 * whatever padding brings the whole to a byte multiple. `field` is its declaration, and `header` what
 * the file says before the packet. */
static int round_trip_under(
  const char *header,
  const char *scalars,
  unsigned lead,
  const char *field,
  uint64_t width,
  bool bit_string,
  bool lsb_first,
  uint64_t *state
) {
  const uint64_t tail = (8 - (lead + width) % 8) % 8;

  char source[512];
  int written = snprintf(source, sizeof(source), "%spkt P {\n  %sid: 8 = 0x5A;\n", header, scalars);
  if (lead > 0) {
    written += snprintf(source + written, sizeof(source) - (size_t)written, "  %slead: %u;\n", scalars, lead);
  }
  written += snprintf(source + written, sizeof(source) - (size_t)written, "  %s\n", field);
  if (tail > 0) {
    written += snprintf(
      source + written, sizeof(source) - (size_t)written, "  %stail: %llu;\n", scalars, (unsigned long long)tail
    );
  }
  snprintf(source + written, sizeof(source) - (size_t)written, "};\n");

  FramedslResult *result = framedsl_load_string(source, "bits.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = framedsl_frame_new(definition);
  FramedslFieldId lead_id;
  FramedslFieldId value_id;
  FramedslFieldId tail_id;
  int failure = 0;

  if (frame == NULL || !framedsl_find(definition, "P.value", &value_id)) {
    failure = __LINE__;
  }

  for (int round = 0; round < 64 && failure == 0; ++round) {
    uint8_t expected[64] = { 0 };
    const uint64_t lead_value = masked(next(state), lead);
    const uint64_t tail_value = masked(next(state), tail);
    uint64_t scalar = 0;
    uint8_t bytes[32];

    reference_scalar(expected, 0, 8, 0x5A);
    if (lead > 0) {
      framedsl_find(definition, "P.lead", &lead_id);
      framedsl_set_u64(frame, lead_id, lead_value);
      reference_scalar(expected, 8, lead, lead_value);
    }
    if (bit_string) {
      for (size_t i = 0; i < width / 8; ++i) {
        bytes[i] = (uint8_t)next(state);
      }
      if (framedsl_set_bytes(frame, value_id, bytes, (size_t)(width / 8)) != FRAMEDSL_OK) {
        failure = __LINE__;
        break;
      }
      reference_bytes(expected, 8 + lead, bytes, (size_t)(width / 8));
    } else {
      scalar = masked(next(state), width);
      if (framedsl_set_u64(frame, value_id, scalar) != FRAMEDSL_OK) {
        failure = __LINE__;
        break;
      }
      if (lsb_first) {
        reference_scalar_lsb_first(expected, 8 + lead, width, scalar);
      } else {
        reference_scalar(expected, 8 + lead, width, scalar);
      }
    }
    if (tail > 0) {
      framedsl_find(definition, "P.tail", &tail_id);
      framedsl_set_u64(frame, tail_id, tail_value);
      reference_scalar(expected, 8 + lead + width, tail, tail_value);
    }

    size_t length = 0;
    const uint8_t *encoded = framedsl_encode(frame, "P", &length, NULL);
    if (encoded == NULL || length != (8 + lead + width + tail) / 8 || memcmp(encoded, expected, length) != 0) {
      failure = __LINE__;
      break;
    }

    /* And back: the decoder must recover exactly what was set, from the reference's bytes. */
    framedsl_frame_reset(frame);
    if (framedsl_decode(frame, expected, length, NULL, NULL) == NULL) {
      failure = __LINE__;
      break;
    }
    if (bit_string) {
      size_t got_length = 0;
      const uint8_t *got = framedsl_get_bytes(frame, value_id, &got_length, NULL);
      if (got == NULL || got_length != width / 8 || memcmp(got, bytes, got_length) != 0) {
        failure = __LINE__;
      }
    } else {
      uint64_t got = 0;
      if (framedsl_get_u64(frame, value_id, &got) != FRAMEDSL_OK || got != scalar) {
        failure = __LINE__;
      }
    }
  }

  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int round_trip(unsigned lead, const char *field, uint64_t width, bool bit_string, uint64_t *state) {
  return round_trip_under("", "", lead, field, width, bit_string, false, state);
}

static int us12_tc01_whole_byte_scalars_at_every_offset(const TestArguments *args) {
  (void)args;
  uint64_t state = 0x9E3779B97F4A7C15u;
  const unsigned widths[] = { 8, 16, 24, 32, 64 };
  for (unsigned lead = 0; lead < 8; ++lead) {
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); ++w) {
      char field[64];
      snprintf(field, sizeof(field), "value: %u;", widths[w]);
      int failure = round_trip(lead, field, widths[w], false, &state);
      if (failure != 0) {
        return failure;
      }
    }
  }
  return 0;
}

static int us12_tc02_odd_width_scalars_at_every_offset(const TestArguments *args) {
  (void)args;
  uint64_t state = 0xD1B54A32D192ED03u;
  const unsigned widths[] = { 1, 3, 7, 9, 13, 17, 31, 33, 63 };
  for (unsigned lead = 0; lead < 8; ++lead) {
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); ++w) {
      char field[64];
      snprintf(field, sizeof(field), "value: %u;", widths[w]);
      int failure = round_trip(lead, field, widths[w], false, &state);
      if (failure != 0) {
        return failure;
      }
    }
  }
  return 0;
}

/* Bit strings: fixed ones wider than a scalar, at every offset, which is the path that became a copy
 * on a boundary and a shift off one. */
static int us12_tc03_bit_strings_at_every_offset(const TestArguments *args) {
  (void)args;
  uint64_t state = 0x94D049BB133111EBu;
  const unsigned bytes[] = { 9, 16, 25 };
  for (unsigned lead = 0; lead < 8; ++lead) {
    for (size_t b = 0; b < sizeof(bytes) / sizeof(*bytes); ++b) {
      char field[64];
      snprintf(field, sizeof(field), "value: %u;", bytes[b] * 8);
      int failure = round_trip(lead, field, bytes[b] * 8, true, &state);
      if (failure != 0) {
        return failure;
      }
    }
  }
  return 0;
}

/* Bit strings off a byte boundary go eight bytes at a time, with a byte loop for what is left (#105):
 * every offset off a boundary, at lengths either side of one word, two and three, under either bit
 * order for the file -- which a bit string ignores, and must go on ignoring. Seven and eight bytes are
 * 56 and 64 bits, which is a scalar rather than a bit string, so those two lengths go through as one. */
static int us12_tc04_bit_strings_around_a_word_off_a_boundary(const TestArguments *args) {
  (void)args;
  uint64_t state = 0xBF58476D1CE4E5B9u;
  const unsigned bytes[] = { 7, 8, 9, 15, 16, 17, 23, 24, 25 };
  /* Under an lsb_first file the scalars around the bit string are written msb_first, which is what the
   * reference lays down; the bit string inherits the file's order, and ignores it. */
  const char *const headers[][2] = {
    { "@bit_order: msb_first;\n", ""                            },
    { "@bit_order: lsb_first;\n", "[ @bit_order: msb_first; ] " },
  };
  for (size_t h = 0; h < sizeof(headers) / sizeof(*headers); ++h) {
    for (unsigned lead = 1; lead < 8; ++lead) {
      for (size_t b = 0; b < sizeof(bytes) / sizeof(*bytes); ++b) {
        const bool bit_string = bytes[b] * 8 > 64;
        char field[64];
        snprintf(field, sizeof(field), "%svalue: %u;", bit_string ? "" : headers[h][1], bytes[b] * 8);
        int failure =
          round_trip_under(headers[h][0], headers[h][1], lead, field, bytes[b] * 8, bit_string, false, &state);
        if (failure != 0) {
          return failure;
        }
      }
    }
  }
  return 0;
}

/* lsb_first scalars are the value reversed and laid down as msb_first ones are (#108): every width that
 * is sub-byte, crosses bytes or fills a word, at every offset, against the bit-at-a-time reference. */
static int us12_tc05_lsb_first_scalars_at_every_offset(const TestArguments *args) {
  (void)args;
  uint64_t state = 0x2545F4914F6CDD1Du;
  const unsigned widths[] = { 1, 3, 7, 8, 9, 12, 13, 16, 17, 24, 31, 32, 33, 48, 63, 64 };
  for (unsigned lead = 0; lead < 8; ++lead) {
    for (size_t w = 0; w < sizeof(widths) / sizeof(*widths); ++w) {
      char field[96];
      snprintf(field, sizeof(field), "[ @bit_order: lsb_first; ] value: %u;", widths[w]);
      int failure = round_trip_under("", "", lead, field, widths[w], false, true, &state);
      if (failure != 0) {
        return failure;
      }
    }
  }
  return 0;
}

void register_user_story12(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us12_tc01_whole_byte_scalars_at_every_offset, "whole-byte scalars match the reference at every offset", false
  );
  ADD_TEST_CASE(
    us12_tc02_odd_width_scalars_at_every_offset, "odd-width scalars match the reference at every offset", false
  );
  ADD_TEST_CASE(us12_tc03_bit_strings_at_every_offset, "bit strings match the reference at every offset", false);
  ADD_TEST_CASE(
    us12_tc04_bit_strings_around_a_word_off_a_boundary,
    "bit strings around a word match the reference off a boundary",
    false
  );
  ADD_TEST_CASE(
    us12_tc05_lsb_first_scalars_at_every_offset, "lsb_first scalars match the reference at every offset", false
  );
}
