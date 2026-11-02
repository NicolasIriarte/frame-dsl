/* Story 33: little-endian bitfields (#147).
 *
 * `little_endian` covers a field of any width: bit j of a field at offset n is bit (n + j) % 8 of byte
 * (n + j) / 8. Fields fill a byte from the bottom and run on into the next, as a packed C struct does on
 * a little-endian processor, and a CAN signal in Intel format. Big-endian fields fill one from the top, so
 * two of different orders cannot share a byte (check 10). */
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

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(Fixture *fixture, const char *source) {
  fixture->result = framedsl_load_string(source, "bits.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void close_fixture(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *dotted) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, dotted, &id);
  return id;
}

static uint64_t read_u64(const Fixture *fixture, const char *dotted) {
  uint64_t value = UINT64_MAX;
  framedsl_get_u64(fixture->frame, id_of(fixture, dotted), &value);
  return value;
}

static bool encodes(FramedslFrame *frame, const char *packet, const uint8_t *expected, size_t length) {
  size_t got = 0;
  FramedslStatus status = FRAMEDSL_OK;
  const uint8_t *bytes = framedsl_encode(frame, packet, &got, &status);
  return status == FRAMEDSL_OK && bytes != NULL && got == length && memcmp(bytes, expected, length) == 0;
}

/* A decode from memory that ends where the frame does, so that reading one byte too far is caught. */
static bool decodes(FramedslFrame *frame, const uint8_t *wire, size_t length) {
  uint8_t *exact = malloc(length);
  memcpy(exact, wire, length);
  size_t consumed = 0;
  const bool decoded = framedsl_decode(frame, exact, length, &consumed, NULL) != NULL && consumed == length;
  free(exact);
  return decoded;
}

static bool refused(const char *source, int check, const char *text) {
  FramedslResult *result = framedsl_load_string(source, "refused.fdsl");
  bool found = false;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diagnostic = framedsl_diagnostic(result, i);
    found = found || (diagnostic->check == check && strstr(diagnostic->message, text) != NULL);
  }
  const bool loaded = framedsl_ok(result);
  framedsl_result_free(result);
  return found && !loaded;
}

static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "loads.fdsl");
  const bool loaded = framedsl_ok(result);
  framedsl_result_free(result);
  return loaded;
}

/* J1939 Address Claimed: eight bytes, a NAME sent little-endian, fields crossing byte boundaries. */
static const char NAME[] =
  "[ @byte_order: little_endian; ]\n"
  "pkt ADDRESS_CLAIMED {\n"
  "  identity:          21;\n"
  "  manufacturer:      11;\n"
  "  ecu_instance:      3;\n"
  "  function_instance: 5;\n"
  "  function:          8;\n"
  "  reserved:          1 := 0;\n"
  "  vehicle_system:    7;\n"
  "  system_instance:   4;\n"
  "  industry_group:    3;\n"
  "  arbitrary:         1;\n"
  "};\n";

static int us33_tc01_a_name_reads_field_by_field_and_back(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture, NAME)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0x45, 0x23, 0x61, 0x55, 0x8D, 0x81, 0x46, 0xA9 };
  EXPECT(decodes(fixture.frame, wire, sizeof(wire)));
  const struct {
    const char *name;
    uint64_t value;
  } cases[] = {
    { "ADDRESS_CLAIMED.identity",          0x12345 },
    { "ADDRESS_CLAIMED.manufacturer",      0x2AB   },
    { "ADDRESS_CLAIMED.ecu_instance",      5       },
    { "ADDRESS_CLAIMED.function_instance", 0x11    },
    { "ADDRESS_CLAIMED.function",          0x81    },
    { "ADDRESS_CLAIMED.reserved",          0       },
    { "ADDRESS_CLAIMED.vehicle_system",    0x23    },
    { "ADDRESS_CLAIMED.system_instance",   9       },
    { "ADDRESS_CLAIMED.industry_group",    2       },
    { "ADDRESS_CLAIMED.arbitrary",         1       },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    EXPECT(read_u64(&fixture, cases[i].name) == cases[i].value);
  }

  framedsl_frame_reset(fixture.frame);
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    if (strcmp(cases[i].name, "ADDRESS_CLAIMED.reserved") != 0) {
      EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, cases[i].name), cases[i].value) == FRAMEDSL_OK);
    }
  }
  EXPECT(encodes(fixture.frame, "ADDRESS_CLAIMED", wire, sizeof(wire)));
  close_fixture(&fixture);
  return failure;
}

struct __attribute__((packed)) Word {
  uint16_t value : 12;
  uint16_t flags : 4;
};

struct __attribute__((packed)) Mixed {
  uint32_t a : 1;
  uint32_t b : 7;
  uint32_t c : 13;
  uint32_t d : 11;
  uint8_t e;
  uint16_t f;
};

/* What the compiler building this test lays out, compared with what the library does with the fields. */
static int us33_tc02_the_bytes_match_a_packed_struct(const TestArguments *args) {
  (void)args;
  if (*(const uint8_t *)&(const uint16_t){ 1 } != 1) {
    return 0; /* a struct compiled on a big-endian host says nothing about this */
  }
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@decode: manual;\n@byte_order: little_endian;\n"
        "pkt WORD { value: 12; flags: 4; };\n"
        "pkt MIXED { a: 1; b: 7; c: 13; d: 11; e: 8; f: 16; };\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  const struct Word word = { .value = 0x123, .flags = 0xA };
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "WORD.value"), 0x123) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "WORD.flags"), 0xA) == FRAMEDSL_OK);
  EXPECT(encodes(fixture.frame, "WORD", (const uint8_t *)&word, sizeof(word)));
  const uint8_t expected[] = { 0x23, 0xA1 };
  EXPECT(encodes(fixture.frame, "WORD", expected, sizeof(expected)));

  uint32_t seed = 0x9E3779B9u;
  for (int i = 0; i < 2000 && failure == 0; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const uint32_t more = seed * 1664525u + 1013904223u;
    const struct Mixed mixed = {
      .a = seed & 1,
      .b = (seed >> 1) & 0x7F,
      .c = (seed >> 8) & 0x1FFF,
      .d = (seed >> 21) & 0x7FF,
      .e = (uint8_t)more,
      .f = (uint16_t)(more >> 8),
    };
    const uint64_t fields[] = { mixed.a, mixed.b, mixed.c, mixed.d, mixed.e, mixed.f };
    const char *const names[] = { "MIXED.a", "MIXED.b", "MIXED.c", "MIXED.d", "MIXED.e", "MIXED.f" };
    for (size_t f = 0; f < 6; ++f) {
      EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, names[f]), fields[f]) == FRAMEDSL_OK);
    }
    EXPECT(encodes(fixture.frame, "MIXED", (const uint8_t *)&mixed, sizeof(mixed)));
    EXPECT(framedsl_decode_as(fixture.frame, "MIXED", (const uint8_t *)&mixed, sizeof(mixed), NULL) == FRAMEDSL_OK);
    for (size_t f = 0; f < 6; ++f) {
      EXPECT(read_u64(&fixture, names[f]) == fields[f]);
    }
  }
  close_fixture(&fixture);
  return failure;
}

/* A field across a boundary, of one bit, and through every width up to a 64-bit one touching nine bytes,
 * in frames that end where they do. */
static int us33_tc03_any_width_round_trips(const TestArguments *args) {
  (void)args;
  int failure = 0;
  for (unsigned before = 1; before < 8 && failure == 0; ++before) {
    for (unsigned width = 1; width <= 64 && failure == 0; ++width) {
      const unsigned total = before + width;
      const unsigned after = (8 - total % 8) % 8;
      char source[256];
      snprintf(
        source,
        sizeof(source),
        "@decode: manual;\n@byte_order: little_endian;\npkt P { a: %u; v: %u; z: %u; };\n",
        before,
        width,
        after == 0 ? 8 : after
      );
      Fixture fixture;
      if (!open_fixture(&fixture, source)) {
        return __LINE__;
      }
      const uint64_t value = width == 64 ? 0xA5C3F00F12345678u : 0xA5C3F00F12345678u & (((uint64_t)1 << width) - 1);
      EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.v"), value) == FRAMEDSL_OK);
      EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.a"), 1) == FRAMEDSL_OK);
      size_t length = 0;
      const uint8_t *bytes = framedsl_encode(fixture.frame, "P", &length, NULL);
      EXPECT(bytes != NULL);
      if (bytes != NULL) {
        uint8_t *exact = malloc(length);
        memcpy(exact, bytes, length);
        framedsl_frame_reset(fixture.frame);
        EXPECT(framedsl_decode_as(fixture.frame, "P", exact, length, NULL) == FRAMEDSL_OK);
        free(exact);
        EXPECT(read_u64(&fixture, "P.v") == value);
        EXPECT(read_u64(&fixture, "P.a") == 1);
      }
      close_fixture(&fixture);
    }
  }
  return failure;
}

/* EEC1, J1939 PGN 61444: a published frame, in the field order its specification lists. */
static int us33_tc04_a_published_can_frame_reads_its_value(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@decode: manual;\n@byte_order: little_endian;\n"
        "cal rpm(raw) = raw * 0.125;\n"
        "pkt EEC1 {\n"
        "  torque_mode:       4;\n"
        "  additional_torque: 4;\n"
        "  demand_percent:    8;\n"
        "  actual_percent:    8;\n"
        "  [ @cal: rpm; ]\n"
        "  engine_speed:      16;\n"
        "  source_address:    8;\n"
        "  starter_mode:      4;\n"
        "  reserved:          4;\n"
        "  engine_demand:     8;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0xFF, 0xFF, 0xFF, 0x68, 0x13, 0xFF, 0xFF, 0xFF };
  EXPECT(framedsl_decode_as(fixture.frame, "EEC1", wire, sizeof(wire), NULL) == FRAMEDSL_OK);
  double rpm = 0;
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "EEC1.engine_speed"), &rpm) == FRAMEDSL_OK && rpm == 621.0);

  /* The two halves of one byte are told apart the way the specification numbers them. */
  const uint8_t nibbles[] = { 0x21, 0, 0, 0, 0, 0, 0x43, 0 };
  EXPECT(framedsl_decode_as(fixture.frame, "EEC1", nibbles, sizeof(nibbles), NULL) == FRAMEDSL_OK);
  EXPECT(read_u64(&fixture, "EEC1.torque_mode") == 1);
  EXPECT(read_u64(&fixture, "EEC1.additional_torque") == 2);
  EXPECT(read_u64(&fixture, "EEC1.starter_mode") == 3);
  EXPECT(read_u64(&fixture, "EEC1.reserved") == 4);
  close_fixture(&fixture);
  return failure;
}

/* Everything that works on a slot does not see the difference. */
static int us33_tc05_a_type_a_curve_an_enum_and_a_condition_still_work(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@byte_order: little_endian;\n"
        "enum Mode: 4 { IDLE; RUN; };\n"
        "cal half(raw) = raw / 2;\n"
        "pkt P {\n"
        "  id:   8 := 0x11;\n"
        "  [ @cal: half; ]\n"
        "  t:    i12;\n"
        "  mode: Mode;\n"
        "  [ @if: valof(t) < 0; ]\n"
        "  cold: 8;\n"
        "  tail: 8;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_i64(fixture.frame, id_of(&fixture, "P.t"), -2) == FRAMEDSL_OK);
  EXPECT(framedsl_set_name(fixture.frame, id_of(&fixture, "P.mode"), "RUN") == FRAMEDSL_OK);
  const uint8_t expected[] = { 0x11, 0xFE, 0x1F, 0x00, 0x00 };
  EXPECT(encodes(fixture.frame, "P", expected, sizeof(expected)));

  EXPECT(decodes(fixture.frame, expected, sizeof(expected)));
  int64_t t = 0;
  double eng = 0;
  const char *name = NULL;
  EXPECT(framedsl_get_i64(fixture.frame, id_of(&fixture, "P.t"), &t) == FRAMEDSL_OK && t == -2);
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "P.t"), &eng) == FRAMEDSL_OK && eng == -1.0);
  EXPECT(framedsl_get_name(fixture.frame, id_of(&fixture, "P.mode"), &name) == FRAMEDSL_OK && strcmp(name, "RUN") == 0);
  bool present = false;
  EXPECT(framedsl_present(fixture.frame, id_of(&fixture, "P.cold"), &present) == FRAMEDSL_OK && present);
  close_fixture(&fixture);
  return failure;
}

/* What identifies a packet is the bytes a frame must have, so a fixed value has to be where it really is. */
static int us33_tc06_a_fixed_field_identifies_where_it_really_is(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@byte_order: little_endian;\n"
        "pkt LOW  { id: 4 := 1; rest: 4; };\n"
        "pkt HIGH { id: 4 := 2; rest: 4; };\n"
        "pkt WIDE { id: 12 := 0x3; rest: 4; };\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t low[] = { 0xF1 };
  const uint8_t high[] = { 0xF2 };
  const char *packet = framedsl_decode(fixture.frame, low, sizeof(low), NULL, NULL);
  EXPECT(packet != NULL && strcmp(packet, "LOW") == 0);
  EXPECT(read_u64(&fixture, "LOW.rest") == 0xF);
  packet = framedsl_decode(fixture.frame, high, sizeof(high), NULL, NULL);
  EXPECT(packet != NULL && strcmp(packet, "HIGH") == 0);
  const uint8_t wide[] = { 0x03, 0x50 };
  packet = framedsl_decode(fixture.frame, wide, sizeof(wide), NULL, NULL);
  EXPECT(packet != NULL && strcmp(packet, "WIDE") == 0);
  EXPECT(read_u64(&fixture, "WIDE.rest") == 5);
  const uint8_t top_nibble[] = { 0x1F };
  FramedslStatus status = FRAMEDSL_OK;
  EXPECT(framedsl_decode(fixture.frame, top_nibble, sizeof(top_nibble), NULL, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_UNRECOGNISED);
  close_fixture(&fixture);

  /* These pin bits in different halves of the byte, so a frame could be both: the other numbering would
   * see the same bits contradict. */
  EXPECT(refused(
    "@byte_order: little_endian;\npkt A { id: 4 := 1; rest: 4; };\npkt B { rest: 4; id: 4 := 2; };\n",
    12,
    "cannot be told apart"
  ));
  return failure;
}

static int us33_tc07_lsb_first_stays_independent(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@byte_order: little_endian;\n"
        "pkt P {\n"
        "  id: 8 := 0x20;\n"
        "  [ @bit_order: lsb_first; ]\n"
        "  r:  4;\n"
        "  n:  4;\n"
        "  [ @bit_order: lsb_first; ]\n"
        "  w:  16;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.r"), 0x1) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.n"), 0x3) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.w"), 0x0102) == FRAMEDSL_OK);
  /* r is 0b0001 reversed in the low half; w keeps what lsb_first with little_endian always gave. */
  const uint8_t expected[] = { 0x20, 0x38, 0x80, 0x40 };
  EXPECT(encodes(fixture.frame, "P", expected, sizeof(expected)));
  EXPECT(decodes(fixture.frame, expected, sizeof(expected)));
  EXPECT(read_u64(&fixture, "P.r") == 1);
  EXPECT(read_u64(&fixture, "P.w") == 0x0102);
  close_fixture(&fixture);
  return failure;
}

static int us33_tc08_a_member_can_be_a_sub_byte_field(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@byte_order: little_endian;\n"
        "block Item { level: 12; mark: 4 := 0xA; low: i4; high: 4 ?= 3; };\n"
        "pkt P { id: 8 := 0x30; items: Item[2]; };\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.items[1].level"), 0x123) == FRAMEDSL_OK);
  EXPECT(framedsl_set_i64(fixture.frame, id_of(&fixture, "P.items[1].low"), -1) == FRAMEDSL_OK);
  const uint8_t expected[] = { 0x30, 0x00, 0xA0, 0x30, 0x23, 0xA1, 0x3F };
  EXPECT(encodes(fixture.frame, "P", expected, sizeof(expected)));
  EXPECT(decodes(fixture.frame, expected, sizeof(expected)));
  EXPECT(read_u64(&fixture, "P.items[1].level") == 0x123);
  EXPECT(read_u64(&fixture, "P.items[1].high") == 3);
  EXPECT(read_u64(&fixture, "P.items[0].mark") == 0xA);
  int64_t low = 0;
  EXPECT(framedsl_get_i64(fixture.frame, id_of(&fixture, "P.items[1].low"), &low) == FRAMEDSL_OK && low == -1);
  close_fixture(&fixture);
  return failure;
}

static int us33_tc09_written_on_a_sub_byte_field_it_loads(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(loads(
    "pkt P {\n  id: 8 := 1;\n  [ @byte_order: little_endian; ]\n  seq: 15;\n  [ @byte_order: little_endian; ]\n  tail: "
    "1;\n};\n"
  ));
  EXPECT(loads("pkt P { id: 8 := 1; [ @byte_order: little_endian; ] block { a: 4; b: 4; }; };\n"));
  return failure;
}

static int us33_tc10_two_orders_cannot_share_a_byte(const TestArguments *args) {
  (void)args;
  int failure = 0;
  /* Written on one, and the other as the file left it. */
  EXPECT(refused("pkt P { id: 8 := 1; [ @byte_order: little_endian; ] a: 4; b: 4; };\n", 10, "shares a byte with 'a'"));
  /* Inherited on one, and written back on the other. */
  EXPECT(refused(
    "@byte_order: little_endian;\npkt P { id: 8 := 1; a: 4; [ @byte_order: big_endian; ] b: 4; };\n", 10, "must agree"
  ));
  /* Across a block's edge, which is nothing to the bytes. */
  EXPECT(refused(
    "[ @byte_order: little_endian; ]\nblock B { a: 4; };\npkt P { id: 8 := 1; b: B; c: 4; };\n", 10, "shares a byte"
  ));
  /* A boundary is what makes either fine. */
  EXPECT(
    loads("pkt P { id: 8 := 1; [ @byte_order: little_endian; ] a: 16; b: 8; [ @byte_order: little_endian; ] c: 8; };\n")
  );
  EXPECT(
    loads("[ @byte_order: little_endian; ]\nblock B { a: 4; b: 4; };\npkt P { id: 8 := 1; b: B; c: 8; d: 4; e: 4; };\n")
  );
  return failure;
}

static int us33_tc11_a_boundary_has_to_be_shown(const TestArguments *args) {
  (void)args;
  int failure = 0;
  /* Past a conditional part, which is whole bytes, a field is on a boundary whether the part is there. */
  EXPECT(loads(
    "pkt P { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] e: 8; [ @byte_order: little_endian; ] block { a: 4; b: 4; }; "
    "};\n"
  ));
  /* Past a width that is whole bytes, the same. */
  EXPECT(
    loads("pkt P { id: 8 := 1; n: 8; d[4]: valof(n) * 8; [ @byte_order: little_endian; ] block { a: 4; b: 4; }; };\n")
  );
  /* Inside a conditional part it is where the part says. */
  EXPECT(refused(
    "pkt P { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] block { a: 4; [ @byte_order: little_endian; ] b: 4; }; };\n",
    10,
    "must agree"
  ));
  /* A width nobody can show to be whole bytes is check 13's, and this says only what it means here. */
  EXPECT(refused(
    "pkt P { id: 8 := 1; n: 8; d[4]: valof(n); [ @byte_order: little_endian; ] a: 8; };\n", 10, "may share a byte"
  ));
  return failure;
}

static int us33_tc12_bytes_go_out_big_endian(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(
    refused("@byte_order: little_endian;\npkt P { id: 8 := 1; a: 4; d: 100; };\n", 10, "bytes, which go out big-endian")
  );
  EXPECT(refused(
    "@byte_order: little_endian;\nblock B { x: 8; };\npkt P { id: 8 := 1; a: 4; b: B[2]; c: 4; };\n",
    10,
    "bytes, which go out big-endian"
  ));
  /* A repetition of little-endian bits has to be where those bits read. */
  EXPECT(refused(
    "[ @byte_order: little_endian; ]\nblock B { x: 4; y: 4; };\npkt P { id: 8 := 1; a: 4; b: B[2]; c: 4; };\n",
    10,
    "must start on a byte boundary"
  ));
  EXPECT(loads(
    "@byte_order: little_endian;\nblock B { x: 8; };\npkt P { id: 8 := 1; a: 4; c: 4; b: B[2]; d[4]: 8 * valof(id); "
    "};\n"
  ));
  return failure;
}

/* Whole bytes, and anything big-endian, are what they were: the bytes are written out here by hand. */
static int us33_tc13_everything_else_keeps_its_bytes(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "pkt P {\n"
        "  id: 8 := 0x40;\n"
        "  a:  3;\n"
        "  b:  13;\n"
        "  [ @byte_order: little_endian; ]\n"
        "  c:  16;\n"
        "  [ @byte_order: little_endian; ]\n"
        "  d:  32;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.a"), 5) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.b"), 0x1234) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.c"), 0xABCD) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.d"), 0x01020304) == FRAMEDSL_OK);
  const uint8_t expected[] = { 0x40, 0xB2, 0x34, 0xCD, 0xAB, 0x04, 0x03, 0x02, 0x01 };
  EXPECT(encodes(fixture.frame, "P", expected, sizeof(expected)));
  EXPECT(decodes(fixture.frame, expected, sizeof(expected)));
  EXPECT(read_u64(&fixture, "P.d") == 0x01020304);
  close_fixture(&fixture);
  return failure;
}

/* A little-endian field past a variable one has no known offset, and the walk works it out per frame. */
static int us33_tc14_past_a_variable_field_it_still_reads(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(
        &fixture,
        "@byte_order: little_endian;\n"
        "pkt P {\n"
        "  id:      8 := 0x41;\n"
        "  n:       8;\n"
        "  d[4]:    valof(n) * 8;\n"
        "  value:   12;\n"
        "  flags:   4;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0x41, 0x02, 0xAA, 0xBB, 0x23, 0xA1 };
  EXPECT(decodes(fixture.frame, wire, sizeof(wire)));
  EXPECT(read_u64(&fixture, "P.value") == 0x123);
  EXPECT(read_u64(&fixture, "P.flags") == 0xA);
  EXPECT(encodes(fixture.frame, "P", wire, sizeof(wire)));
  close_fixture(&fixture);
  return failure;
}

void register_user_story33(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us33_tc01_a_name_reads_field_by_field_and_back, "a J1939 NAME reads each field and encodes back", false
  );
  ADD_TEST_CASE(us33_tc02_the_bytes_match_a_packed_struct, "the bytes match a packed C struct", false);
  ADD_TEST_CASE(us33_tc03_any_width_round_trips, "every width at every offset round-trips", false);
  ADD_TEST_CASE(us33_tc04_a_published_can_frame_reads_its_value, "a published CAN frame reads its value", false);
  ADD_TEST_CASE(
    us33_tc05_a_type_a_curve_an_enum_and_a_condition_still_work, "a type, a curve, an enum and a condition work", false
  );
  ADD_TEST_CASE(
    us33_tc06_a_fixed_field_identifies_where_it_really_is, "a fixed field identifies where it really is", false
  );
  ADD_TEST_CASE(us33_tc07_lsb_first_stays_independent, "lsb_first reverses a value in either order", false);
  ADD_TEST_CASE(us33_tc08_a_member_can_be_a_sub_byte_field, "a repetition's member can be a sub-byte field", false);
  ADD_TEST_CASE(us33_tc09_written_on_a_sub_byte_field_it_loads, "@byte_order written on a sub-byte field loads", false);
  ADD_TEST_CASE(
    us33_tc10_two_orders_cannot_share_a_byte, "two orders cannot share a byte, written or inherited", false
  );
  ADD_TEST_CASE(us33_tc11_a_boundary_has_to_be_shown, "a boundary that cannot be shown is refused", false);
  ADD_TEST_CASE(us33_tc12_bytes_go_out_big_endian, "a bit string and a repetition count as big-endian", false);
  ADD_TEST_CASE(us33_tc13_everything_else_keeps_its_bytes, "whole bytes and big-endian fields keep their bytes", false);
  ADD_TEST_CASE(us33_tc14_past_a_variable_field_it_still_reads, "past a variable field it still reads", false);
}
