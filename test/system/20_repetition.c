/* Story 20: repetition (#55) -- `name: Block[count];`, through the public API.
 *
 * A repetition is walked as one run of elements, and reached element by element: by name,
 * "PKT.samples[3].value", or with framedsl_element() in a loop. Each case builds a frame, encodes it,
 * compares the bytes with ones written out by hand, and decodes them back -- a layout is only right if
 * both directions agree with the wire. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool fixture_open(Fixture *fixture, const char *source) {
  fixture->result = framedsl_load_string(source, "repetition.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static bool set(Fixture *fixture, const char *name, uint64_t value) {
  FramedslFieldId id;
  return framedsl_find(fixture->definition, name, &id) && framedsl_set_u64(fixture->frame, id, value) == FRAMEDSL_OK;
}

static bool get(Fixture *fixture, const char *name, uint64_t *value) {
  FramedslFieldId id;
  return framedsl_find(fixture->definition, name, &id) && framedsl_get_u64(fixture->frame, id, value) == FRAMEDSL_OK;
}

static bool encodes_to(Fixture *fixture, const char *packet, const uint8_t *want, size_t want_length) {
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, NULL);
  return bytes != NULL && length == want_length && memcmp(bytes, want, length) == 0;
}

/* The first diagnostic's check number, or -1 when the load succeeded. */
static int first_check(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "repetition.fdsl");
  int check =
    framedsl_ok(result) || framedsl_diagnostic_count(result) == 0 ? -1 : framedsl_diagnostic(result, 0)->check;
  framedsl_result_free(result);
  return check;
}

static const char *const SAMPLES =
  "block Sample {\n"
  "  channel: 8;\n"
  "  value:   16;\n"
  "};\n"
  "pkt TM {\n"
  "  id:          8 := 0x10;\n"
  "  count:       8;\n"
  "  samples[12]: Sample[valof(count)];\n"
  "  last:        Sample[2];\n"
  "};\n";

static int us20_tc01_a_constant_count_encodes_and_decodes(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t want[] = { 0x10, 0x00, 0x03, 0x00, 0x0C, 0x04, 0x00, 0x0D };
  if (!set(&fixture, "TM.last[0].channel", 3) || !set(&fixture, "TM.last[0].value", 12)
      || !set(&fixture, "TM.last[1].channel", 4) || !set(&fixture, "TM.last[1].value", 13)) {
    failure = __LINE__;
  } else if (!encodes_to(&fixture, "TM", want, sizeof(want))) {
    failure = __LINE__;
  }

  /* Back, into a frame that never saw the values. */
  Fixture reader;
  uint64_t value = 0;
  if (failure == 0 && fixture_open(&reader, SAMPLES)) {
    size_t consumed = 0;
    const char *name = framedsl_decode(reader.frame, want, sizeof(want), &consumed, NULL);
    if (name == NULL || strcmp(name, "TM") != 0 || consumed != sizeof(want)) {
      failure = __LINE__;
    } else if (!get(&reader, "TM.last[1].value", &value) || value != 13) {
      failure = __LINE__;
    }
    fixture_close(&reader);
  }
  fixture_close(&fixture);
  return failure;
}

static int us20_tc02_a_counted_repetition_carries_as_many_as_the_count_says(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t want[] = { 0x10, 0x02, 0x01, 0x00, 0x0A, 0x02, 0x00, 0x0B, 0, 0, 0, 0, 0, 0 };
  if (!set(&fixture, "TM.count", 2) || !set(&fixture, "TM.samples[0].channel", 1)
      || !set(&fixture, "TM.samples[0].value", 10) || !set(&fixture, "TM.samples[1].channel", 2)
      || !set(&fixture, "TM.samples[1].value", 11)) {
    failure = __LINE__;
  } else if (!encodes_to(&fixture, "TM", want, sizeof(want))) {
    failure = __LINE__;
  }

  /* An element past the count is in the frame's storage, not on the wire. */
  if (failure == 0 && (!set(&fixture, "TM.samples[3].value", 99) || !encodes_to(&fixture, "TM", want, sizeof(want)))) {
    failure = __LINE__;
  }

  Fixture reader;
  if (failure == 0 && fixture_open(&reader, SAMPLES)) {
    FramedslFieldId samples;
    size_t count = 0;
    uint64_t value = 0;
    if (framedsl_decode(reader.frame, want, sizeof(want), NULL, NULL) == NULL) {
      failure = __LINE__;
    } else if (!framedsl_find(reader.definition, "TM.samples", &samples)
               || framedsl_count(reader.frame, samples, &count) != FRAMEDSL_OK || count != 2) {
      failure = __LINE__;
    } else if (!get(&reader, "TM.samples[1].value", &value) || value != 11) {
      failure = __LINE__;
    }
    fixture_close(&reader);
  }
  fixture_close(&fixture);
  return failure;
}

/* The same bound a valof() width must have: the reservation, in bytes, is what the arena sets aside. */
static int us20_tc03_a_counted_repetition_needs_a_bound(const TestArguments *args) {
  (void)args;
  const char *unbounded = "block S {\n  a: 8;\n};\npkt P {\n  id: 8 := 1;\n  n:  8;\n  xs: S[valof(n)];\n};\n";
  const char *redundant = "block S {\n  a: 8;\n};\npkt P {\n  id:    8 := 1;\n  xs[4]: S[2];\n};\n";
  if (first_check(unbounded) != 5) {
    return __LINE__;
  }
  return first_check(redundant) == 14 ? 0 : __LINE__;
}

static int us20_tc04_a_count_past_the_bound_is_refused(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  /* samples[12] holds four 3-byte elements. */
  const uint8_t five[] = { 0x10, 0x05, 1, 0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 0, 0, 0, 0, 0, 0 };
  FramedslStatus status = FRAMEDSL_OK;
  FramedslFieldId samples;
  size_t count = 0;
  size_t length = 0;
  if (framedsl_decode(fixture.frame, five, sizeof(five), NULL, &status) != NULL || status != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  } else if (!set(&fixture, "TM.count", 5) || framedsl_encode(fixture.frame, "TM", &length, &status) != NULL
             || status != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  } else if (!framedsl_find(fixture.definition, "TM.samples", &samples)
             || framedsl_count(fixture.frame, samples, &count) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

/* By name with an index, or found once without one and indexed in a loop: the same field either way. */
static int us20_tc05_each_element_is_reachable_by_index(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  FramedslFieldId value;
  FramedslFieldId element;
  if (!framedsl_find(fixture.definition, "TM.samples.value", &value)) {
    failure = __LINE__;
  }
  for (size_t i = 0; i < 4 && failure == 0; ++i) {
    if (framedsl_element(fixture.definition, value, i, &element) != FRAMEDSL_OK
        || framedsl_set_u64(fixture.frame, element, 100 + i) != FRAMEDSL_OK) {
      failure = __LINE__;
    }
  }
  for (size_t i = 0; i < 4 && failure == 0; ++i) {
    char name[64];
    uint64_t got = 0;
    snprintf(name, sizeof(name), "TM.samples[%zu].value", i);
    if (!get(&fixture, name, &got) || got != 100 + i) {
      failure = __LINE__;
    }
  }
  /* A neighbour's bits are its own. */
  uint64_t channel = 1;
  if (failure == 0 && (!get(&fixture, "TM.samples[2].channel", &channel) || channel != 0)) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

static int us20_tc06_what_is_not_an_element_is_refused(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  FramedslFieldId id;
  FramedslFieldId value;
  FramedslFieldId count;
  uint64_t got = 0;
  if (framedsl_find(fixture.definition, "TM.samples[4].value", &id)
      || framedsl_find(fixture.definition, "TM.last[2].value", &id)) {
    failure = __LINE__; /* past the room there is */
  } else if (framedsl_find(fixture.definition, "TM.samples[1].nope", &id)
             || framedsl_find(fixture.definition, "TM.samples[x].value", &id)
             || framedsl_find(fixture.definition, "TM.samples[1]", &id)) {
    failure = __LINE__;
  } else if (!framedsl_find(fixture.definition, "TM.samples.value", &value)
             || framedsl_element(fixture.definition, value, 4, &id) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, value, &got) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* no element chosen yet */
  } else if (!framedsl_find(fixture.definition, "TM.count", &count)
             || framedsl_element(fixture.definition, count, 0, &id) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* not a repetition's field */
  }
  fixture_close(&fixture);
  return failure;
}

/* A fixed value in the block is on every element on the wire, whatever the caller left in the slot;
 * orders and calibrations are the block's, per element. */
static const char *const TYPED =
  "cal half(raw) = raw / 2;\n"
  "block Reading {\n"
  "  marker: 4 := 0xA;\n"
  "  [ @bit_order: lsb_first; ]\n"
  "  flags:  4;\n"
  "  [ @byte_order: little_endian; @cal: half; ]\n"
  "  value:  16;\n"
  "};\n"
  "pkt TM {\n"
  "  id:       8 := 0x20;\n"
  "  readings: Reading[2];\n"
  "};\n";

static int us20_tc07_each_element_keeps_its_blocks_fixed_values_orders_and_curves(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, TYPED)) {
    return __LINE__;
  }
  int failure = 0;
  FramedslFieldId value;
  double eng = 0;
  /* flags 0b0001 lsb_first is 1000 on the wire; 0x1234 little-endian is 34 12. */
  const uint8_t want[] = { 0x20, 0xA8, 0x34, 0x12, 0xA0, 0x10, 0x00 };
  if (!set(&fixture, "TM.readings[0].flags", 1) || !set(&fixture, "TM.readings[0].value", 0x1234)
      || !framedsl_find(fixture.definition, "TM.readings[1].value", &value)
      || framedsl_set_eng(fixture.frame, value, 8) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (!encodes_to(&fixture, "TM", want, sizeof(want))) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, want, sizeof(want), NULL, NULL) == NULL
             || framedsl_get_eng(fixture.frame, value, &eng) != FRAMEDSL_OK || eng != 8) {
    failure = __LINE__;
  }
  uint64_t marker = 0;
  if (failure == 0 && (!get(&fixture, "TM.readings[1].marker", &marker) || marker != 0xA)) {
    failure = __LINE__; /* decoded, so the wire's value */
  }
  fixture_close(&fixture);
  return failure;
}

static int us20_tc08_a_repeated_block_is_one_size_and_whole_bytes(const TestArguments *args) {
  (void)args;
  const char *varying =
    "block M {\n  n:    8;\n  t[4]: valof(n) * 8;\n};\n"
    "pkt P {\n  id: 8 := 1;\n  ms: M[2];\n};\n";
  const char *nibble = "block N {\n  a: 4;\n};\npkt P {\n  id: 8 := 1;\n  ns: N[2];\n};\n";
  const char *override = "block B {\n  k: 8;\n};\npkt P {\n  id: 8 := 1;\n  bs: B[2] { k := 1; };\n};\n";
  if (first_check(varying) != 32 || first_check(nibble) != 32) {
    return __LINE__;
  }
  return first_check(override) == 33 ? 0 : __LINE__;
}

/* Inside a named block, the count is that instance's: `hdr.count`, not some other `count`. */
static int us20_tc09_a_repetition_inside_an_instance_counts_by_that_instance(const TestArguments *args) {
  (void)args;
  const char *source =
    "block S {\n  a: 8;\n};\n"
    "block Body {\n  count: 8;\n  xs[4]: S[valof(count)];\n};\n"
    "pkt P {\n  id:    8 := 1;\n  count: 8;\n  body:  Body;\n};\n";
  Fixture fixture;
  if (!fixture_open(&fixture, source)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t want[] = { 0x01, 0x07, 0x02, 0xAA, 0xBB };
  if (!set(&fixture, "P.count", 7) || !set(&fixture, "P.body.count", 2) || !set(&fixture, "P.body.xs[0].a", 0xAA)
      || !set(&fixture, "P.body.xs[1].a", 0xBB)) {
    failure = __LINE__;
  } else if (!encodes_to(&fixture, "P", want, sizeof(want))) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

static int us20_tc10_introspection_describes_the_element(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  size_t packet = 0;
  size_t field = 0;
  FramedslField info;
  FramedslField member;
  if (!framedsl_find_packet(fixture.definition, "TM", &packet)
      || !framedsl_find_field(fixture.definition, packet, "samples", &field)
      || !framedsl_field(fixture.definition, packet, field, &info)) {
    failure = __LINE__;
  } else if (info.element_bits != 24 || info.member_count != 2 || !info.variable || info.max_bits != 96
             || info.is_scalar) {
    failure = __LINE__;
  } else if (!framedsl_member(fixture.definition, packet, field, 1, &member) || strcmp(member.name, "value") != 0
             || member.offset_bits != 8 || member.width_bits != 16 || !member.is_scalar) {
    failure = __LINE__;
  } else if (framedsl_member(fixture.definition, packet, field, 2, &member)
             || framedsl_member(fixture.definition, packet, 0, 0, &member)) {
    failure = __LINE__; /* past the members, and not a repetition */
  }
  fixture_close(&fixture);
  return failure;
}

/* The repetition as a whole is the bytes that arrived, like any bit string. */
static int us20_tc11_the_whole_repetition_reads_as_bytes(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SAMPLES)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t frame[] = { 0x10, 0x01, 0x07, 0x01, 0x02, 0, 0, 0, 0, 0, 0 };
  FramedslFieldId samples;
  size_t length = 0;
  const uint8_t *bytes = NULL;
  if (framedsl_decode(fixture.frame, frame, sizeof(frame), NULL, NULL) == NULL
      || !framedsl_find(fixture.definition, "TM.samples", &samples)) {
    failure = __LINE__;
  } else if ((bytes = framedsl_get_bytes(fixture.frame, samples, &length, NULL)) == NULL || length != 3
             || bytes[0] != 0x07 || bytes[2] != 0x02) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

/* The worked example, as the tool sees it: -d prints each element by the name framedsl_find() takes. */
static int us20_tc12_the_tool_decodes_elements(const TestArguments *args) {
  (void)args;
  char command[1024];
  snprintf(
    command, sizeof(command), "%s %s/sensor_samples.fdsl -d '30 00 00 01 A3 23 01' 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR
  );
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return __LINE__;
  }
  char output[4096] = { 0 };
  size_t used = fread(output, 1, sizeof(output) - 1, pipe);
  output[used] = '\0';
  int status = pclose(pipe);
  if (status != 0 || strstr(output, "samples[0].channel 3 (0x3)") == NULL) {
    return __LINE__;
  }
  return strstr(output, "samples[0].reading 291 (0x123)") != NULL && strstr(output, "samples        1 element") != NULL
           ? 0
           : __LINE__;
}

/* A reference for tc13, a bit at a time: stream bit i is bit 7 - (i % 8) of byte i / 8. `lsb_first`
 * puts the value's bit j at the field's bit j; `swap` reverses its bytes first, as little_endian does. */
static void reference_put(uint8_t *out, uint64_t offset, uint64_t width, uint64_t value, bool lsb_first, bool swap) {
  if (swap) {
    uint64_t swapped = 0;
    for (uint64_t byte = 0; byte < width / 8; ++byte) {
      swapped = swapped << 8 | ((value >> (byte * 8)) & 0xFF);
    }
    value = swapped;
  }
  for (uint64_t j = 0; j < width; ++j) {
    uint64_t bit = lsb_first ? (value >> j) & 1 : (value >> (width - 1 - j)) & 1;
    if (bit != 0) {
      out[(offset + j) / 8] |= (uint8_t)(1u << (7 - (offset + j) % 8));
    }
  }
}

/* tc13's frame, its elements `lead` bits after a 16-bit header. */
static int every_shape_of_member(unsigned lead) {
  static const struct {
    const char *name;
    uint64_t offset;
    uint64_t width;
    bool lsb_first;
    bool swap;
    bool fixed;
    uint64_t value;
  } members[] = {
    { "a", 0,  8,  false, false, false, 0    },
    { "w", 8,  16, false, true,  false, 0    },
    { "m", 24, 3,  false, false, true,  5    },
    { "l", 27, 7,  true,  false, false, 0    },
    { "x", 34, 13, false, false, false, 0    },
    { "s", 47, 9,  true,  false, false, 0    },
    { "f", 56, 8,  false, false, true,  0xA5 },
  };
  Fixture fixture = { 0 };
  Fixture receiver = { 0 };
  char source[512];
  snprintf(
    source,
    sizeof(source),
    "block E {\n"
    "  a: 8;\n"
    "  [ @byte_order: little_endian; ] w: 16;\n"
    "  m: 3 := 5;\n"
    "  [ @bit_order: lsb_first; ] l: 7;\n"
    "  x: 13;\n"
    "  [ @bit_order: lsb_first; ] s: 9;\n"
    "  f: 8 := 0xA5;\n"
    "};\n"
    "pkt P { id: 8 := 0x5A; count: 8; pre: %u; items[64]: E[valof(count)]; post: %u; };\n",
    lead,
    lead
  );
  if (!fixture_open(&fixture, source) || !fixture_open(&receiver, source)) {
    fixture_close(&fixture);
    fixture_close(&receiver);
    return __LINE__;
  }
  int failure = 0;
  const uint64_t count = 6;
  uint8_t want[80] = { 0 };
  reference_put(want, 0, 8, 0x5A, false, false);
  reference_put(want, 8, 8, count, false, false);
  reference_put(want, 16, lead, 0x9, false, false);
  if (!set(&fixture, "P.count", count) || !set(&fixture, "P.pre", 0x9) || !set(&fixture, "P.post", 0x6)) {
    failure = __LINE__;
  }
  uint64_t state = 0x9E3779B97F4A7C15u;
  uint64_t values[6][7];
  for (uint64_t e = 0; e < count && failure == 0; ++e) {
    uint8_t element[8] = { 0 };
    for (size_t m = 0; m < sizeof(members) / sizeof(*members); ++m) {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      uint64_t value = members[m].fixed ? members[m].value : state & ((UINT64_C(1) << members[m].width) - 1);
      values[e][m] = value;
      reference_put(element, members[m].offset, members[m].width, value, members[m].lsb_first, members[m].swap);
      if (!members[m].fixed) {
        char name[32];
        FramedslFieldId any;
        FramedslFieldId id;
        snprintf(name, sizeof(name), "P.items.%s", members[m].name);
        if (!framedsl_find(fixture.definition, name, &any)
            || framedsl_element(fixture.definition, any, (size_t)e, &id) != FRAMEDSL_OK
            || framedsl_set_u64(fixture.frame, id, value) != FRAMEDSL_OK) {
          failure = __LINE__;
        }
      }
    }
    reference_put(
      want,
      16 + lead + e * 64,
      64,
      (uint64_t)element[0] << 56 | (uint64_t)element[1] << 48 | (uint64_t)element[2] << 40 | (uint64_t)element[3] << 32
        | (uint64_t)element[4] << 24 | (uint64_t)element[5] << 16 | (uint64_t)element[6] << 8 | element[7],
      false,
      false
    );
  }
  reference_put(want, 16 + lead + count * 64, lead, 0x6, false, false);
  const size_t length = (size_t)(16 + 2 * lead + count * 64) / 8;
  if (failure == 0 && !encodes_to(&fixture, "P", want, length)) {
    failure = __LINE__;
  }

  /* Decoded into another frame, every member reads back -- fixed ones as their fixed values. */
  if (failure == 0 && framedsl_decode(receiver.frame, want, length, NULL, NULL) == NULL) {
    failure = __LINE__;
  }
  for (uint64_t e = 0; e < count && failure == 0; ++e) {
    for (size_t m = 0; m < sizeof(members) / sizeof(*members) && failure == 0; ++m) {
      char name[32];
      uint64_t got = 0;
      snprintf(name, sizeof(name), "P.items[%llu].%s", (unsigned long long)e, members[m].name);
      if (!get(&receiver, name, &got) || got != values[e][m]) {
        failure = __LINE__;
      }
    }
  }
  fixture_close(&fixture);
  fixture_close(&receiver);
  return failure;
}

/* Every shape of member in one element, reached through the accessors and stamped by encode (#101):
 * whole bytes on a boundary, little-endian, fixed values sub-byte and whole, lsb_first across a byte,
 * and a 13-bit field off a boundary. The elements sit on a byte boundary in the frame, where encode
 * stamps fixed values as masked bytes, and four bits off one, where it writes them member by member.
 * Each element's bytes, and so the frame, against a reference laid down a bit at a time. */
static int us20_tc13_every_shape_of_member_matches_a_reference(const TestArguments *args) {
  (void)args;
  int failure = every_shape_of_member(8);
  return failure != 0 ? failure : every_shape_of_member(4);
}

void register_user_story20(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us20_tc01_a_constant_count_encodes_and_decodes, "a constant count encodes and decodes", false);
  ADD_TEST_CASE(
    us20_tc02_a_counted_repetition_carries_as_many_as_the_count_says,
    "a counted repetition carries what the count says",
    false
  );
  ADD_TEST_CASE(
    us20_tc03_a_counted_repetition_needs_a_bound, "a counted repetition needs a bound, a constant one none", false
  );
  ADD_TEST_CASE(us20_tc04_a_count_past_the_bound_is_refused, "a count past the bound is refused both ways", false);
  ADD_TEST_CASE(us20_tc05_each_element_is_reachable_by_index, "each element is reachable by name or by index", false);
  ADD_TEST_CASE(us20_tc06_what_is_not_an_element_is_refused, "what is not an element is refused", false);
  ADD_TEST_CASE(
    us20_tc07_each_element_keeps_its_blocks_fixed_values_orders_and_curves,
    "each element keeps its block's fixed values, orders and curves",
    false
  );
  ADD_TEST_CASE(
    us20_tc08_a_repeated_block_is_one_size_and_whole_bytes, "a repeated block is one size, whole bytes, unfixed", false
  );
  ADD_TEST_CASE(
    us20_tc09_a_repetition_inside_an_instance_counts_by_that_instance,
    "inside an instance, the instance's count counts",
    false
  );
  ADD_TEST_CASE(
    us20_tc10_introspection_describes_the_element, "introspection describes the element and its fields", false
  );
  ADD_TEST_CASE(
    us20_tc11_the_whole_repetition_reads_as_bytes, "the whole repetition reads as the bytes that arrived", false
  );
  ADD_TEST_CASE(us20_tc12_the_tool_decodes_elements, "-d prints each element by name", false);
  ADD_TEST_CASE(
    us20_tc13_every_shape_of_member_matches_a_reference,
    "every shape of member matches a bit-at-a-time reference",
    false
  );
}
