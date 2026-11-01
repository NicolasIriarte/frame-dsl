/* Story 31: a field needs a type as well as a width (#10).
 *
 * A signed value or a float is a pattern of at most 64 bits, so the wire and the slot do not change. The
 * type applies where a value is read or written, where it enters a curve, and where a width or a
 * condition reads it. Everything else in these cases is there to prove the bits stayed where they were. */
#include <math.h>
#include <stdio.h>
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

static const char DEFINITION[] =
  "cal celsius(raw) = raw * 0.0625;\n"
  "cal bent(raw) = raw * raw inverse(v) = v / 2;\n"
  "block Item { level: i4; gain: 4; };\n"
  "pkt TM_THERMAL {\n"
  "  id:   8 := 0x30;\n"
  "  [ @cal: celsius; ]\n"
  "  temp: i16;\n"
  "  gain: f32;\n"
  "  wide: f64;\n"
  "};\n"
  "pkt TM_ODD {\n"
  "  id:  8 := 0x31;\n"
  "  a:   i3;\n"
  "  b:   i13;\n"
  "  low: i64;\n"
  "};\n"
  "pkt TM_ITEMS {\n"
  "  id:    8 := 0x32;\n"
  "  items: Item[2];\n"
  "};\n"
  "pkt TM_VALUES {\n"
  "  id:     8 := 0x33;\n"
  "  offset: i8 := -1;\n"
  "  temp:   i16 ?= -160;\n"
  "  gain:   f32 ?= 1.5;\n"
  "  whole:  f32 ?= 2;\n"
  "  plain:  8;\n"
  "};\n"
  "pkt TM_SIZED {\n"
  "  id:      8 := 0x34;\n"
  "  size:    i8;\n"
  "  data[8]: (valof(size) + 2) * 8;\n"
  "};\n"
  "pkt TM_CONDITION {\n"
  "  id: 8 := 0x35;\n"
  "  t:  i8;\n"
  "  [ @if: valof(t) < -1; ]\n"
  "  cold: 8;\n"
  "  [ @if: valof(t) & 0x80 != 0; ]\n"
  "  sign: 8;\n"
  "  tail: 8;\n"
  "};\n";

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(Fixture *fixture) {
  fixture->result = framedsl_load_string(DEFINITION, "typed.fdsl");
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

static int64_t signed_at(const Fixture *fixture, const char *dotted) {
  int64_t value = INT64_MAX;
  framedsl_get_i64(fixture->frame, id_of(fixture, dotted), &value);
  return value;
}

static double float_at(const Fixture *fixture, const char *dotted) {
  double value = -12345;
  framedsl_get_f64(fixture->frame, id_of(fixture, dotted), &value);
  return value;
}

static bool encodes(FramedslFrame *frame, const char *packet, const uint8_t *expected, size_t length) {
  size_t got = 0;
  FramedslStatus status = FRAMEDSL_OK;
  const uint8_t *bytes = framedsl_encode(frame, packet, &got, &status);
  return status == FRAMEDSL_OK && bytes != NULL && got == length && memcmp(bytes, expected, length) == 0;
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

static int us31_tc01_a_signed_field_round_trips(const TestArguments *args) {
  (void)args;
  FramedslResult *result =
    framedsl_load_string("pkt P { id: 8 := 1; a: i8; b: i16; c: i32; d: i64; };\n", "round.fdsl");
  FramedslFrame *frame = framedsl_frame_new(framedsl_definition(result));
  const FramedslDefinition *definition = framedsl_definition(result);
  const struct {
    const char *name;
    int64_t low;
    int64_t high;
  } cases[] = {
    { "P.a", INT8_MIN,  INT8_MAX  },
    { "P.b", INT16_MIN, INT16_MAX },
    { "P.c", INT32_MIN, INT32_MAX },
    { "P.d", INT64_MIN, INT64_MAX },
  };
  int failure = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    FramedslFieldId id;
    EXPECT(framedsl_find(definition, cases[i].name, &id));
    for (int which = 0; which < 3; ++which) {
      const int64_t value = which == 0 ? cases[i].low : which == 1 ? cases[i].high : -1;
      EXPECT(framedsl_set_i64(frame, id, value) == FRAMEDSL_OK);
      size_t length = 0;
      const uint8_t *bytes = framedsl_encode(frame, "P", &length, NULL);
      uint8_t wire[64];
      memcpy(wire, bytes, length);
      EXPECT(framedsl_decode(frame, wire, length, NULL, NULL) != NULL);
      int64_t read = 0;
      EXPECT(framedsl_get_i64(frame, id, &read) == FRAMEDSL_OK && read == value);
    }
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* Sign extension from widths that are no whole number of bytes, off a byte boundary. */
static int us31_tc02_sign_extends_from_any_width(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_i64(fixture.frame, id_of(&fixture, "TM_ODD.a"), -4) == FRAMEDSL_OK);
  EXPECT(framedsl_set_i64(fixture.frame, id_of(&fixture, "TM_ODD.b"), -1) == FRAMEDSL_OK);
  const uint8_t expected[] = { 0x31, 0x9F, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0 };
  EXPECT(encodes(fixture.frame, "TM_ODD", expected, sizeof(expected)));
  FramedslStatus status = FRAMEDSL_ERROR_TRUNCATED;
  EXPECT(framedsl_decode(fixture.frame, expected, sizeof(expected), NULL, &status) != NULL);
  EXPECT(signed_at(&fixture, "TM_ODD.a") == -4);
  EXPECT(signed_at(&fixture, "TM_ODD.b") == -1);
  close_fixture(&fixture);
  return failure;
}

static int us31_tc03_a_float_round_trips(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_f64(fixture.frame, id_of(&fixture, "TM_THERMAL.gain"), 1.5) == FRAMEDSL_OK);
  EXPECT(framedsl_set_f64(fixture.frame, id_of(&fixture, "TM_THERMAL.wide"), -2.25) == FRAMEDSL_OK);
  const uint8_t expected[] = { 0x30, 0x00, 0x00, 0x3F, 0xC0, 0x00, 0x00, 0xC0, 0x02, 0, 0, 0, 0, 0, 0 };
  EXPECT(encodes(fixture.frame, "TM_THERMAL", expected, sizeof(expected)));
  EXPECT(framedsl_decode(fixture.frame, expected, sizeof(expected), NULL, NULL) != NULL);
  EXPECT(float_at(&fixture, "TM_THERMAL.gain") == 1.5);
  EXPECT(float_at(&fixture, "TM_THERMAL.wide") == -2.25);
  close_fixture(&fixture);
  return failure;
}

static int us31_tc04_the_widths_a_type_may_have(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt P { id: 8 := 1; x: i0; };\n", 44, "i1 to i64"));
  EXPECT(refused("pkt P { id: 8 := 1; x: i65; };\n", 44, "i1 to i64"));
  EXPECT(refused("pkt P { id: 8 := 1; x: f16; };\n", 44, "f32 or f64"));
  EXPECT(refused("pkt P { id: 8 := 1; x: i08; };\n", 44, "i1 to i64"));
  EXPECT(loads("pkt P { id: 8 := 1; a: i1; b: i7; c: i64; d: f32; e: f64; };\n"));
  return failure;
}

static int us31_tc05_every_order_holds(const TestArguments *args) {
  (void)args;
  const char *const orders[] = {
    "[ @bit_order: msb_first; @byte_order: big_endian; ]",
    "[ @bit_order: msb_first; @byte_order: little_endian; ]",
    "[ @bit_order: lsb_first; @byte_order: big_endian; ]",
    "[ @bit_order: lsb_first; @byte_order: little_endian; ]",
  };
  int failure = 0;
  for (size_t i = 0; i < sizeof(orders) / sizeof(*orders); ++i) {
    char source[512];
    snprintf(source, sizeof(source), "pkt P { id: 8 := 1; %s block { s: i16; f: f32; }; };\n", orders[i]);
    FramedslResult *result = framedsl_load_string(source, "orders.fdsl");
    const FramedslDefinition *definition = framedsl_definition(result);
    FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
    FramedslFieldId s;
    FramedslFieldId f;
    EXPECT(frame != NULL && framedsl_find(definition, "P.s", &s) && framedsl_find(definition, "P.f", &f));
    if (frame != NULL) {
      EXPECT(framedsl_set_i64(frame, s, -160) == FRAMEDSL_OK);
      EXPECT(framedsl_set_f64(frame, f, -0.5) == FRAMEDSL_OK);
      size_t length = 0;
      const uint8_t *bytes = framedsl_encode(frame, "P", &length, NULL);
      uint8_t wire[16];
      memcpy(wire, bytes, length);
      framedsl_frame_reset(frame);
      EXPECT(framedsl_decode(frame, wire, length, NULL, NULL) != NULL);
      int64_t read_s = 0;
      double read_f = 0;
      EXPECT(framedsl_get_i64(frame, s, &read_s) == FRAMEDSL_OK && read_s == -160);
      EXPECT(framedsl_get_f64(frame, f, &read_f) == FRAMEDSL_OK && read_f == -0.5);
    }
    framedsl_frame_free(frame);
    framedsl_result_free(result);
  }
  return failure;
}

static int us31_tc06_a_type_cannot_be_a_blocks_name_and_can_be_a_fields(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("block i16 { x: 8; };\npkt P { id: 8 := 1; };\n", 1, "reads as a type"));
  EXPECT(refused("block f32 { x: 8; };\npkt P { id: 8 := 1; };\n", 1, "reads as a type"));
  EXPECT(refused("enum i16: 8 { A; };\npkt P { id: 8 := 1; };\n", 1, "reads as a type"));
  EXPECT(refused("enum f32: 8 { A; };\npkt P { id: 8 := 1; };\n", 1, "reads as a type"));
  EXPECT(loads("pkt P { i16: 8 := 1; f32: i16; };\n"));
  return failure;
}

/* The raw accessors see the pattern, on every scalar, and a test can send any of them. */
static int us31_tc07_raw_bits_are_never_refused_for_a_type(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId temp = id_of(&fixture, "TM_THERMAL.temp");
  const FramedslFieldId gain = id_of(&fixture, "TM_THERMAL.gain");
  EXPECT(framedsl_set_i64(fixture.frame, temp, -160) == FRAMEDSL_OK);
  EXPECT(framedsl_set_f64(fixture.frame, gain, 1.5) == FRAMEDSL_OK);
  uint64_t bits = 0;
  EXPECT(framedsl_get_u64(fixture.frame, temp, &bits) == FRAMEDSL_OK && bits == 0xFF60);
  EXPECT(framedsl_get_u64(fixture.frame, gain, &bits) == FRAMEDSL_OK && bits == 0x3FC00000);

  EXPECT(framedsl_set_u64(fixture.frame, temp, 0xFF60) == FRAMEDSL_OK);
  EXPECT(signed_at(&fixture, "TM_THERMAL.temp") == -160);
  EXPECT(framedsl_set_u64(fixture.frame, gain, 0x7FC00001) == FRAMEDSL_OK);
  EXPECT(isnan(float_at(&fixture, "TM_THERMAL.gain")));
  EXPECT(framedsl_get_u64(fixture.frame, gain, &bits) == FRAMEDSL_OK && bits == 0x7FC00001);
  close_fixture(&fixture);
  return failure;
}

static int us31_tc08_the_typed_accessors_refuse(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId temp = id_of(&fixture, "TM_THERMAL.temp");
  const FramedslFieldId gain = id_of(&fixture, "TM_THERMAL.gain");
  const FramedslFieldId plain = id_of(&fixture, "TM_VALUES.plain");
  const FramedslFieldId fixed = id_of(&fixture, "TM_VALUES.offset");
  int64_t number = 0;
  double real = 0;

  EXPECT(framedsl_set_i64(fixture.frame, gain, 1) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_set_i64(fixture.frame, plain, 1) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_get_i64(fixture.frame, gain, &number) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_set_f64(fixture.frame, temp, 1) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_get_f64(fixture.frame, plain, &real) == FRAMEDSL_ERROR_FIELD);

  EXPECT(framedsl_set_i64(fixture.frame, fixed, 5) == FRAMEDSL_ERROR_FIXED);
  /* Out of range on a fixed field is still the fixed answer, as #140 orders them. */
  EXPECT(framedsl_set_i64(fixture.frame, fixed, 500) == FRAMEDSL_ERROR_FIXED);

  EXPECT(framedsl_set_i64(fixture.frame, temp, -5) == FRAMEDSL_OK);
  EXPECT(framedsl_set_i64(fixture.frame, temp, 40000) == FRAMEDSL_ERROR_RANGE);
  EXPECT(framedsl_set_i64(fixture.frame, temp, -40000) == FRAMEDSL_ERROR_RANGE);
  EXPECT(signed_at(&fixture, "TM_THERMAL.temp") == -5);
  close_fixture(&fixture);
  return failure;
}

static int us31_tc09_an_f32_rounds_and_passes_what_is_not_a_number(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId gain = id_of(&fixture, "TM_THERMAL.gain");
  EXPECT(framedsl_set_f64(fixture.frame, gain, 0.1) == FRAMEDSL_OK);
  EXPECT(float_at(&fixture, "TM_THERMAL.gain") == (double)0.1f);

  EXPECT(framedsl_set_f64(fixture.frame, gain, 2.5) == FRAMEDSL_OK);
  EXPECT(framedsl_set_f64(fixture.frame, gain, 1e39) == FRAMEDSL_ERROR_RANGE);
  EXPECT(float_at(&fixture, "TM_THERMAL.gain") == 2.5);

  EXPECT(framedsl_set_f64(fixture.frame, gain, INFINITY) == FRAMEDSL_OK);
  EXPECT(isinf(float_at(&fixture, "TM_THERMAL.gain")));
  EXPECT(framedsl_set_f64(fixture.frame, gain, -INFINITY) == FRAMEDSL_OK);
  EXPECT(float_at(&fixture, "TM_THERMAL.gain") < 0);
  EXPECT(framedsl_set_f64(fixture.frame, gain, NAN) == FRAMEDSL_OK);
  EXPECT(isnan(float_at(&fixture, "TM_THERMAL.gain")));
  close_fixture(&fixture);
  return failure;
}

static int us31_tc10_a_curve_sees_the_value(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId temp = id_of(&fixture, "TM_THERMAL.temp");
  double reading = 0;
  EXPECT(framedsl_set_i64(fixture.frame, temp, -160) == FRAMEDSL_OK);
  EXPECT(framedsl_get_eng(fixture.frame, temp, &reading) == FRAMEDSL_OK && reading == -10.0);
  EXPECT(framedsl_set_eng(fixture.frame, temp, -5.0) == FRAMEDSL_OK);
  EXPECT(signed_at(&fixture, "TM_THERMAL.temp") == -80);
  /* To the nearest, half away from zero: -5.03125 C is -80.5 counts. */
  EXPECT(framedsl_set_eng(fixture.frame, temp, -5.03125) == FRAMEDSL_OK);
  EXPECT(signed_at(&fixture, "TM_THERMAL.temp") == -81);
  EXPECT(framedsl_set_eng(fixture.frame, temp, 3000.0) == FRAMEDSL_ERROR_RANGE);
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "TM_THERMAL.gain"), &reading) == FRAMEDSL_ERROR_FIELD);
  close_fixture(&fixture);

  /* A float is not rounded. */
  FramedslResult *result =
    framedsl_load_string("cal half(raw) = raw / 2;\npkt P { id: 8 := 1; [ @cal: half; ] g: f32; };\n", "float.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = framedsl_frame_new(definition);
  FramedslFieldId gain;
  EXPECT(framedsl_find(definition, "P.g", &gain));
  EXPECT(framedsl_set_eng(frame, gain, 0.75) == FRAMEDSL_OK);
  double raw = 0;
  EXPECT(framedsl_get_f64(frame, gain, &raw) == FRAMEDSL_OK && raw == 1.5);
  EXPECT(framedsl_get_eng(frame, gain, &reading) == FRAMEDSL_OK && reading == 0.75);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int us31_tc11_values_in_a_file(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(signed_at(&fixture, "TM_VALUES.offset") == -1);
  EXPECT(signed_at(&fixture, "TM_VALUES.temp") == -160);
  EXPECT(float_at(&fixture, "TM_VALUES.gain") == 1.5);
  EXPECT(float_at(&fixture, "TM_VALUES.whole") == 2.0);
  const uint8_t expected[] = { 0x33, 0xFF, 0xFF, 0x60, 0x3F, 0xC0, 0, 0, 0x40, 0, 0, 0, 0 };
  EXPECT(encodes(fixture.frame, "TM_VALUES", expected, sizeof(expected)));
  close_fixture(&fixture);

  EXPECT(refused("pkt P { id: 8 := 1; x: i8 := 0xFF; };\n", 8, "cannot hold 255"));
  EXPECT(refused("pkt P { id: 8 := 1; x: i8 ?= -129; };\n", 8, "cannot hold -129"));
  EXPECT(refused("pkt P { id: 8 := 1; x: i8 := 1.5; };\n", 8, "fraction"));
  EXPECT(refused("pkt P { id: 8 := 1; x: f32 := 0x3FC00000; };\n", 8, "decimal"));
  EXPECT(refused("pkt P { id: 8 := 1; x: f32 ?= 0b101; };\n", 8, "decimal"));
  EXPECT(refused("pkt P { id: 8 := 1; x: f32 ?= 1e39; };\n", 8, "cannot hold"));
  EXPECT(refused("pkt P { id: 8 := 1; x: 8 := -1; };\n", 8, "unsigned"));
  EXPECT(refused("pkt P { id: 8 := 1; x: 8 ?= 1.5; };\n", 8, "fraction"));
  EXPECT(refused("enum E: 8 { A; };\npkt P { id: 8 := 1; x: E := -1; };\n", 8, "unsigned"));
  EXPECT(refused("pkt P { id: 8 := 1; x: i8 := A; };\n", 39, "not typed by an enum"));
  EXPECT(loads("pkt P { id: 8 := 1; x: i8 := -128; y: i8 := 127; z: i1 := -1; w: i1 := 0; pad: 6; };\n"));
  EXPECT(loads("block B { x: i8 := 1; y: f32 ?= 0; };\npkt P { id: 8 := 1; b: B { x := -1; y ?= -0.5; }; };\n"));
  EXPECT(refused("block B { x: i8; };\npkt P { id: 8 := 1; b: B { x := 200; }; };\n", 8, "cannot hold 200"));
  return failure;
}

/* A fixed value identifies a packet by its pattern, so -1 and 0xFF are the same bits. */
static int us31_tc12_a_fixed_value_identifies_by_its_pattern(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { id: i8 := -1; };\npkt B { id: 8 := 0xFF; };\n", 12, ""));
  EXPECT(loads("pkt A { id: i8 := -1; };\npkt B { id: 8 := 0xFE; };\n"));
  FramedslResult *result = framedsl_load_string("pkt A { id: i8 := -1; x: 8; };\n", "pattern.fdsl");
  FramedslFrame *frame = framedsl_frame_new(framedsl_definition(result));
  const uint8_t wire[] = { 0xFF, 0x01 };
  const char *packet = framedsl_decode(frame, wire, sizeof(wire), NULL, NULL);
  EXPECT(packet != NULL && strcmp(packet, "A") == 0);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int us31_tc13_a_condition_compares_as_signed_and_masks_as_bits(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId t = id_of(&fixture, "TM_CONDITION.t");
  EXPECT(framedsl_set_i64(fixture.frame, t, -5) == FRAMEDSL_OK);
  const uint8_t both[] = { 0x35, 0xFB, 0x00, 0x00, 0x00 };
  EXPECT(encodes(fixture.frame, "TM_CONDITION", both, sizeof(both)));

  /* -1 has the top bit and is not below -1. */
  EXPECT(framedsl_set_i64(fixture.frame, t, -1) == FRAMEDSL_OK);
  const uint8_t sign_only[] = { 0x35, 0xFF, 0x00, 0x00 };
  EXPECT(encodes(fixture.frame, "TM_CONDITION", sign_only, sizeof(sign_only)));

  EXPECT(framedsl_set_i64(fixture.frame, t, 5) == FRAMEDSL_OK);
  const uint8_t neither[] = { 0x35, 0x05, 0x00 };
  EXPECT(encodes(fixture.frame, "TM_CONDITION", neither, sizeof(neither)));
  close_fixture(&fixture);

  EXPECT(refused("pkt P { id: 8 := 1; t: i8; [ @if: valof(t) == 200; ] x: 8; };\n", 8, "can never be 200"));
  EXPECT(refused("pkt P { id: 8 := 1; t: i8; [ @if: valof(t) == -200; ] x: 8; };\n", 8, "can never be -200"));
  EXPECT(refused("pkt P { id: 8 := 1; t: 8; [ @if: valof(t) < -1; ] x: 8; };\n", 8, "negative"));
  EXPECT(refused("pkt P { id: 8 := 1; t: i8; [ @if: valof(t) & 0x80 == -1; ] x: 8; };\n", 8, "negative"));
  EXPECT(refused("pkt P { id: 8 := 1; t: 8; [ @if: valof(t) == 1.5; ] x: 8; };\n", 8, "fraction"));
  return failure;
}

static int us31_tc14_valof_reads_a_signed_field_and_no_float(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  /* A term below zero is fine on the way to a width: -1 + 2 is one byte. */
  const uint8_t one[] = { 0x34, 0xFF, 0xAA };
  FramedslStatus status = FRAMEDSL_ERROR_FIELD;
  EXPECT(framedsl_decode(fixture.frame, one, sizeof(one), NULL, &status) != NULL && status == FRAMEDSL_OK);

  /* -3 + 2 is not a width. */
  const uint8_t below[] = { 0x34, 0xFD, 0xAA };
  EXPECT(framedsl_decode(fixture.frame, below, sizeof(below), NULL, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_RANGE);
  close_fixture(&fixture);

  EXPECT(refused("pkt P { id: 8 := 1; g: f32; d[4]: valof(g); };\n", 16, "float"));
  EXPECT(refused("block B { x: 8; };\npkt P { id: 8 := 1; g: f32; b[4]: B[valof(g)]; };\n", 16, "float"));
  EXPECT(refused("pkt P { id: 8 := 1; g: f32; [ @if: valof(g) == 1; ] x: 8; };\n", 16, "float"));
  return failure;
}

/* Typed members of a repetition, and what the definition reports. */
static int us31_tc15_a_member_and_a_description_say_their_kind(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId level = id_of(&fixture, "TM_ITEMS.items[1].level");
  EXPECT(framedsl_set_i64(fixture.frame, level, -2) == FRAMEDSL_OK);
  EXPECT(signed_at(&fixture, "TM_ITEMS.items[1].level") == -2);
  const uint8_t expected[] = { 0x32, 0x00, 0xE0 };
  EXPECT(encodes(fixture.frame, "TM_ITEMS", expected, sizeof(expected)));

  size_t packet = 0;
  size_t index = 0;
  FramedslField field;
  EXPECT(framedsl_find_packet(fixture.definition, "TM_THERMAL", &packet));
  EXPECT(framedsl_find_field(fixture.definition, packet, "temp", &index));
  EXPECT(framedsl_field(fixture.definition, packet, index, &field) && field.kind == FRAMEDSL_KIND_SIGNED);
  EXPECT(field.width_bits == 16 && field.is_scalar);
  EXPECT(framedsl_find_field(fixture.definition, packet, "gain", &index));
  EXPECT(framedsl_field(fixture.definition, packet, index, &field) && field.kind == FRAMEDSL_KIND_FLOAT);
  EXPECT(framedsl_find_field(fixture.definition, packet, "id", &index));
  EXPECT(framedsl_field(fixture.definition, packet, index, &field) && field.kind == FRAMEDSL_KIND_UNSIGNED);

  EXPECT(framedsl_find_packet(fixture.definition, "TM_ITEMS", &packet));
  EXPECT(framedsl_find_field(fixture.definition, packet, "items", &index));
  EXPECT(framedsl_member(fixture.definition, packet, index, 0, &field) && field.kind == FRAMEDSL_KIND_SIGNED);
  EXPECT(framedsl_member(fixture.definition, packet, index, 1, &field) && field.kind == FRAMEDSL_KIND_UNSIGNED);

  /* Repeating a type directly is #124's. */
  EXPECT(refused("pkt P { id: 8 := 1; x: i16[4]; };\n", 41, "only a block can be repeated"));
  EXPECT(refused("pkt P { id: 8 := 1; i16; };\n", 41, "name the field"));
  close_fixture(&fixture);
  return failure;
}

/* What the tool prints, each field by its type and then its pattern. */
static int us31_tc16_the_tool_prints_a_field_by_its_type(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/typed.fdsl", args->tmp_dir);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return __LINE__;
  }
  fputs(DEFINITION, handle);
  fclose(handle);

  int failure = 0;
  char command[2048];
  char output[4096];
  snprintf(
    command, sizeof(command), "%s %s -d '30 FF 60 3F C0 00 00 3F F8 00 00 00 00 00 00' 2>&1", FRAMEDSL_BIN, path
  );
  FILE *pipe = popen(command, "r");
  size_t used = pipe != NULL ? fread(output, 1, sizeof(output) - 1, pipe) : 0;
  output[used] = '\0';
  if (pipe != NULL) {
    pclose(pipe);
  }
  EXPECT(strstr(output, "    temp           -160 (0xff60) = -10 [celsius]\n") != NULL);
  EXPECT(strstr(output, "    gain           1.5 (0x3fc00000)\n") != NULL);
  EXPECT(strstr(output, "    wide           1.5 (0x3ff8000000000000)\n") != NULL);

  /* Enough digits to read back exactly. */
  snprintf(
    command, sizeof(command), "%s %s -d '30 00 00 3D CC CC CD 3F B9 99 99 99 99 99 9A' 2>&1", FRAMEDSL_BIN, path
  );
  pipe = popen(command, "r");
  used = pipe != NULL ? fread(output, 1, sizeof(output) - 1, pipe) : 0;
  output[used] = '\0';
  if (pipe != NULL) {
    pclose(pipe);
  }
  EXPECT(strstr(output, "    gain           0.100000001 (0x3dcccccd)\n") != NULL);
  EXPECT(strstr(output, "    wide           0.10000000000000001 (0x3fb999999999999a)\n") != NULL);
  return failure;
}

void register_user_story31(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us31_tc01_a_signed_field_round_trips, "a signed field round-trips, the most negative included", false);
  ADD_TEST_CASE(us31_tc02_sign_extends_from_any_width, "sign extension from widths that are not whole bytes", false);
  ADD_TEST_CASE(us31_tc03_a_float_round_trips, "a float round-trips", false);
  ADD_TEST_CASE(us31_tc04_the_widths_a_type_may_have, "i0, i65 and f16 are refused by check 44", false);
  ADD_TEST_CASE(us31_tc05_every_order_holds, "typed fields under both bit orders and both byte orders", false);
  ADD_TEST_CASE(
    us31_tc06_a_type_cannot_be_a_blocks_name_and_can_be_a_fields,
    "no block or enum of a type's name; a field may",
    false
  );
  ADD_TEST_CASE(
    us31_tc07_raw_bits_are_never_refused_for_a_type, "raw accessors see the pattern, a NaN included", false
  );
  ADD_TEST_CASE(us31_tc08_the_typed_accessors_refuse, "typed accessors refuse another type, fixed, or range", false);
  ADD_TEST_CASE(
    us31_tc09_an_f32_rounds_and_passes_what_is_not_a_number, "an f32 rounds, and passes NaN and infinity", false
  );
  ADD_TEST_CASE(us31_tc10_a_curve_sees_the_value, "a curve sees the value, in both directions", false);
  ADD_TEST_CASE(us31_tc11_values_in_a_file, "values in a file are held to the field's type", false);
  ADD_TEST_CASE(us31_tc12_a_fixed_value_identifies_by_its_pattern, "a fixed value identifies by its pattern", false);
  ADD_TEST_CASE(
    us31_tc13_a_condition_compares_as_signed_and_masks_as_bits, "a condition compares as signed, a mask as bits", false
  );
  ADD_TEST_CASE(us31_tc14_valof_reads_a_signed_field_and_no_float, "valof() reads a signed field, and no float", false);
  ADD_TEST_CASE(us31_tc15_a_member_and_a_description_say_their_kind, "a field and a member report their kind", false);
  ADD_TEST_CASE(us31_tc16_the_tool_prints_a_field_by_its_type, "the tool prints a field by its type", false);
}
