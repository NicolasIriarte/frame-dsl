/* Story 23: conditional fields (#12) -- `[ @if: valof(name) == value; ]` in front of a field, a block's
 * use or an anonymous block.
 *
 * Through the public API and the binary: a conditional part is on the wire only when its condition
 * holds, wherever it sits and however it is nested; a field the frame does not carry reads as absent
 * from every getter, and can still be set; what follows it is found in both cases; nothing allocates;
 * and the file is refused, each time for its own check, when a condition cannot be decided, could read
 * a field that is not there, or would leave a frame without a byte form or a way to be recognised. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "allocations.h"
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

static bool fixture_open(Fixture *fixture, const char *source) {
  fixture->result = framedsl_load_string(source, "conditions.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *name) {
  FramedslFieldId id = { .owner = 0, .packet = 0xFFFF, .field = 0xFFFF };
  framedsl_find(fixture->definition, name, &id);
  return id;
}

static FramedslStatus set(Fixture *fixture, const char *name, uint64_t value) {
  return framedsl_set_u64(fixture->frame, id_of(fixture, name), value);
}

/* The value of `name`, or UINT64_MAX when it cannot be read -- absent among the reasons. */
static uint64_t get(const Fixture *fixture, const char *name) {
  uint64_t value = 0;
  return framedsl_get_u64(fixture->frame, id_of(fixture, name), &value) == FRAMEDSL_OK ? value : UINT64_MAX;
}

static FramedslStatus get_status(const Fixture *fixture, const char *name) {
  uint64_t value = 0;
  return framedsl_get_u64(fixture->frame, id_of(fixture, name), &value);
}

static bool present(const Fixture *fixture, const char *name) {
  bool here = false;
  return framedsl_present(fixture->frame, id_of(fixture, name), &here) == FRAMEDSL_OK && here;
}

static size_t parse_hex(const char *hex, uint8_t *out, size_t capacity) {
  size_t count = 0;
  int high = -1;
  for (const char *c = hex; *c != '\0' && count < capacity; ++c) {
    int digit = *c >= '0' && *c <= '9'   ? *c - '0'
                : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10
                : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
                                         : -1;
    if (digit < 0) {
      continue;
    }
    if (high < 0) {
      high = digit;
    } else {
      out[count++] = (uint8_t)(high << 4 | digit);
      high = -1;
    }
  }
  return count;
}

/* Whether `packet` encodes to the bytes written in `hex`, spaces allowed. */
static bool encodes(Fixture *fixture, const char *packet, const char *hex) {
  uint8_t want[256];
  size_t want_length = parse_hex(hex, want, sizeof(want));
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, NULL);
  return bytes != NULL && length == want_length && memcmp(bytes, want, length) == 0;
}

/* Decodes `hex`; the packet's name, or NULL. `consumed` and `status` may be NULL. */
static const char *decodes(Fixture *fixture, const char *hex, size_t *consumed, FramedslStatus *status) {
  uint8_t bytes[256];
  size_t length = parse_hex(hex, bytes, sizeof(bytes));
  return framedsl_decode(fixture->frame, bytes, length, consumed, status);
}

static bool decodes_as(Fixture *fixture, const char *hex, const char *packet) {
  size_t consumed = 0;
  uint8_t bytes[256];
  size_t length = parse_hex(hex, bytes, sizeof(bytes));
  const char *name = framedsl_decode(fixture->frame, bytes, length, &consumed, NULL);
  return name != NULL && strcmp(name, packet) == 0 && consumed == length;
}

/* The diagnostic `source` is refused with for check `wanted`, or NULL. Valid until `*result` is freed. */
static const FramedslDiagnostic *refusal(const char *source, int wanted, FramedslResult **result) {
  *result = framedsl_load_string(source, "conditions.fdsl");
  if (framedsl_ok(*result)) {
    return NULL;
  }
  for (size_t i = 0; i < framedsl_diagnostic_count(*result); ++i) {
    const FramedslDiagnostic *diagnostic = framedsl_diagnostic(*result, i);
    if (diagnostic->check == wanted && diagnostic->severity == FRAMEDSL_SEVERITY_ERROR) {
      return diagnostic;
    }
  }
  return NULL;
}

/* Whether `source` is refused for check `wanted`, with `text` in the message when it is not NULL. */
static bool refused(const char *source, int wanted, const char *text) {
  FramedslResult *result = NULL;
  const FramedslDiagnostic *diagnostic = refusal(source, wanted, &result);
  bool found = diagnostic != NULL && (text == NULL || strstr(diagnostic->message, text) != NULL);
  framedsl_result_free(result);
  return found;
}

static bool write_file(const TestArguments *args, const char *name, const char *content) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static const char *const BASIC =
  "pkt P {\n"
  "  id:   8 = 0x10;\n"
  "  flag: 8;\n"
  "  [ @if: valof(flag) == 1; ]\n"
  "  ext:  16;\n"
  "  tail: 8;\n"
  "};\n";

static const char *const SHAPES =
  "block Pair { a: 8; b: 8; };\n"
  "block Item { v: 8; };\n"
  "pkt S {\n"
  "  id: 8 = 0x20;\n"
  "  m:  8;\n"
  "  [ @if: valof(m) == 1; ] named: Pair;\n"
  "  [ @if: valof(m) == 2; ] Pair;\n"
  "  [ @if: valof(m) == 3; ] block { c: 8; d: 8; };\n"
  "  n:  8;\n"
  "  [ @if: valof(m) == 4; ] items[4]: Item[valof(n)];\n"
  "  end: 8;\n"
  "};\n";

/* ---- On the wire, or not ------------------------------------------------------------------------ */

static int us23_tc01_a_conditional_field_is_on_the_wire_only_when_it_holds(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, BASIC)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;

  EXPECT(set(&fixture, "P.ext", 0x1234) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "P.tail", 0xAB) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "P", "10 00 AB"));
  EXPECT(set(&fixture, "P.flag", 1) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "P", "10 01 12 34 AB"));
  EXPECT(set(&fixture, "P.flag", 2) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "P", "10 02 AB"));

  framedsl_frame_reset(fixture.frame);
  EXPECT(decodes_as(&fixture, "10 01 BE EF CD", "P"));
  EXPECT(get(&fixture, "P.ext") == 0xBEEF && get(&fixture, "P.tail") == 0xCD);
  EXPECT(decodes_as(&fixture, "10 00 CD", "P"));
  EXPECT(get_status(&fixture, "P.ext") == FRAMEDSL_ERROR_ABSENT && get(&fixture, "P.tail") == 0xCD);
  EXPECT(decodes_as(&fixture, "10 FF CD", "P"));
  EXPECT(!present(&fixture, "P.ext"));

  fixture_close(&fixture);
  return failure;
}

static int us23_tc02_every_kind_of_declaration_can_be_conditional(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SHAPES)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  FramedslFieldId v = id_of(&fixture, "S.items.v");
  FramedslFieldId element;

  EXPECT(set(&fixture, "S.named.a", 0xA1) == FRAMEDSL_OK && set(&fixture, "S.named.b", 0xB1) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "S.a", 0xA2) == FRAMEDSL_OK && set(&fixture, "S.b", 0xB2) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "S.c", 0xC3) == FRAMEDSL_OK && set(&fixture, "S.d", 0xD3) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "S.end", 0xEE) == FRAMEDSL_OK);
  EXPECT(framedsl_element(fixture.definition, v, 0, &element) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, element, 0x11) == FRAMEDSL_OK);
  EXPECT(framedsl_element(fixture.definition, v, 1, &element) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, element, 0x22) == FRAMEDSL_OK);

  /* n says two items, but the items are absent until m is 4: n is on the wire, they are not. */
  EXPECT(set(&fixture, "S.n", 2) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "S", "20 00 02 EE"));
  EXPECT(set(&fixture, "S.n", 0) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "S.m", 1) == FRAMEDSL_OK && encodes(&fixture, "S", "20 01 A1 B1 00 EE"));
  EXPECT(set(&fixture, "S.m", 2) == FRAMEDSL_OK && encodes(&fixture, "S", "20 02 A2 B2 00 EE"));
  EXPECT(set(&fixture, "S.m", 3) == FRAMEDSL_OK && encodes(&fixture, "S", "20 03 C3 D3 00 EE"));
  EXPECT(set(&fixture, "S.m", 4) == FRAMEDSL_OK && set(&fixture, "S.n", 2) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "S", "20 04 02 11 22 EE"));

  /* And back: each shape decodes present when its value is there, and absent otherwise. */
  const struct {
    const char *hex;
    bool named, pasted, anonymous, items;
  } frames[] = {
    { "20 00 00 EE",       false, false, false, false },
    { "20 01 A1 B1 00 EE", true,  false, false, false },
    { "20 02 A2 B2 00 EE", false, true,  false, false },
    { "20 03 C3 D3 00 EE", false, false, true,  false },
    { "20 04 02 11 22 EE", false, false, false, true  },
  };
  for (size_t i = 0; i < sizeof(frames) / sizeof(*frames); ++i) {
    framedsl_frame_reset(fixture.frame);
    EXPECT(decodes_as(&fixture, frames[i].hex, "S"));
    EXPECT(present(&fixture, "S.named.a") == frames[i].named && present(&fixture, "S.named.b") == frames[i].named);
    EXPECT(present(&fixture, "S.a") == frames[i].pasted && present(&fixture, "S.b") == frames[i].pasted);
    EXPECT(present(&fixture, "S.c") == frames[i].anonymous && present(&fixture, "S.d") == frames[i].anonymous);
    EXPECT(present(&fixture, "S.items") == frames[i].items && present(&fixture, "S.items[1].v") == frames[i].items);
    EXPECT(get(&fixture, "S.end") == 0xEE && present(&fixture, "S.n"));
  }
  EXPECT(get(&fixture, "S.items[0].v") == 0x11 && get(&fixture, "S.items[1].v") == 0x22);

  fixture_close(&fixture);
  return failure;
}

/* Nested groups AND their conditions, and a group's condition is not its neighbours'. */
static int us23_tc03_nested_conditions_all_have_to_hold(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt N {\n"
        "  id: 8 = 0x30;\n"
        "  a:  4;\n"
        "  b:  4;\n"
        "  [ @if: valof(a) == 1; ]\n"
        "  block {\n"
        "    x: 8;\n"
        "    [ @if: valof(b) != 0; ] y: 8;\n"
        "  };\n"
        "  [ @if: valof(b) != 0; ] z: 8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  EXPECT(set(&fixture, "N.x", 0xAA) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "N.y", 0xBB) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "N.z", 0xCC) == FRAMEDSL_OK);

  const struct {
    uint64_t a, b;
    const char *hex;
  } cases[] = {
    { 0, 0, "30 00"          },
    { 1, 0, "30 10 AA"       },
    { 0, 1, "30 01 CC"       },
    { 1, 1, "30 11 AA BB CC" },
    { 2, 7, "30 27 CC"       },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    EXPECT(set(&fixture, "N.a", cases[i].a) == FRAMEDSL_OK && set(&fixture, "N.b", cases[i].b) == FRAMEDSL_OK);
    EXPECT(encodes(&fixture, "N", cases[i].hex));
  }
  framedsl_frame_reset(fixture.frame);
  EXPECT(decodes_as(&fixture, "30 01 CC", "N"));
  EXPECT(!present(&fixture, "N.x") && !present(&fixture, "N.y") && get(&fixture, "N.z") == 0xCC);
  EXPECT(decodes_as(&fixture, "30 11 01 02 03", "N"));
  EXPECT(get(&fixture, "N.x") == 1 && get(&fixture, "N.y") == 2 && get(&fixture, "N.z") == 3);
  EXPECT(decodes_as(&fixture, "30 10 05", "N"));
  EXPECT(get(&fixture, "N.x") == 5 && !present(&fixture, "N.y") && !present(&fixture, "N.z"));

  FramedslField y;
  size_t packet = 0;
  size_t index = 0;
  EXPECT(framedsl_find_packet(fixture.definition, "N", &packet));
  EXPECT(framedsl_find_field(fixture.definition, packet, "y", &index));
  EXPECT(framedsl_field(fixture.definition, packet, index, &y));
  EXPECT(y.condition != NULL && strcmp(y.condition, "valof(a) == 1 && valof(b) != 0") == 0);

  fixture_close(&fixture);
  return failure;
}

static int us23_tc04_an_enum_field_is_compared_by_name(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "enum Mode: 4 { OFF; ON; TEST = 9; };\n"
        "pkt E {\n"
        "  id:   8 = 0x40;\n"
        "  mode: Mode;\n"
        "  pad:  4;\n"
        "  [ @if: valof(mode) == TEST; ] t: 8;\n"
        "  [ @if: valof(mode) != OFF; ] o: 8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  FramedslFieldId mode = id_of(&fixture, "E.mode");
  EXPECT(set(&fixture, "E.t", 0x77) == FRAMEDSL_OK && set(&fixture, "E.o", 0x88) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "E", "40 00"));
  EXPECT(framedsl_set_name(fixture.frame, mode, "ON") == FRAMEDSL_OK && encodes(&fixture, "E", "40 10 88"));
  EXPECT(framedsl_set_name(fixture.frame, mode, "TEST") == FRAMEDSL_OK && encodes(&fixture, "E", "40 90 77 88"));
  /* A value the enum does not list is still a value: not OFF, so `o` is there; not TEST, so `t` is not. */
  EXPECT(framedsl_set_u64(fixture.frame, mode, 5) == FRAMEDSL_OK && encodes(&fixture, "E", "40 50 88"));
  EXPECT(decodes_as(&fixture, "40 90 01 02", "E") && get(&fixture, "E.t") == 1 && get(&fixture, "E.o") == 2);
  fixture_close(&fixture);
  return failure;
}

/* What follows a conditional part is found whether the part was there or not -- sub-byte fields, a
 * part starting mid-byte, and a counted payload after it. */
static int us23_tc05_fields_after_a_conditional_part_are_found_in_both_cases(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt O {\n"
        "  id:  8 = 0x50;\n"
        "  f:   1;\n"
        "  g:   3;\n"
        "  [ @if: valof(f) == 1; ] block { h: 4; i: 4; };\n"
        "  j:   4;\n"
        "  len: 8;\n"
        "  data[8]: valof(len) * 8;\n"
        "  k:   8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  const uint8_t cc[] = { 0xCC };
  EXPECT(set(&fixture, "O.g", 5) == FRAMEDSL_OK && set(&fixture, "O.j", 0xA) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "O.h", 3) == FRAMEDSL_OK && set(&fixture, "O.i", 7) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "O.len", 1) == FRAMEDSL_OK && set(&fixture, "O.k", 0xDD) == FRAMEDSL_OK);
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "O.data"), cc, 1) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "O", "50 5A 01 CC DD")); /* 0 101 1010 */
  EXPECT(set(&fixture, "O.f", 1) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "O", "50 D3 7A 01 CC DD")); /* 1 101 0011 0111 1010 */

  framedsl_frame_reset(fixture.frame);
  EXPECT(decodes_as(&fixture, "50 D3 7A 02 C1 C2 DD", "O"));
  EXPECT(get(&fixture, "O.h") == 3 && get(&fixture, "O.i") == 7 && get(&fixture, "O.j") == 0xA);
  EXPECT(framedsl_byte_length(fixture.frame, id_of(&fixture, "O.data")) == 2 && get(&fixture, "O.k") == 0xDD);
  EXPECT(decodes_as(&fixture, "50 5A 00 EE", "O"));
  EXPECT(!present(&fixture, "O.h") && get(&fixture, "O.j") == 0xA && get(&fixture, "O.k") == 0xEE);

  /* The part starts at a known offset; nothing after its first field does. */
  size_t packet = 0;
  FramedslField field;
  EXPECT(framedsl_find_packet(fixture.definition, "O", &packet));
  EXPECT(framedsl_field(fixture.definition, packet, 3, &field) && strcmp(field.name, "h") == 0);
  EXPECT(field.offset_known && field.offset_bits == 4 + 8 && field.condition != NULL);
  EXPECT(framedsl_field(fixture.definition, packet, 4, &field) && !field.offset_known);
  EXPECT(framedsl_field(fixture.definition, packet, 5, &field) && !field.offset_known && field.condition == NULL);
  fixture_close(&fixture);
  return failure;
}

/* ---- Absent ---------------------------------------------------------------------------------------- */

static const char *const GETTERS =
  "cal half(raw) = raw / 2;\n"
  "enum M: 8 { A; B; };\n"
  "block It { v: 8; };\n"
  "pkt G {\n"
  "  id: 8 = 0x60;\n"
  "  on: 8;\n"
  "  [ @if: valof(on) == 1; ]\n"
  "  block {\n"
  "    s:      8;\n"
  "    [ @cal: half; ] c: 8;\n"
  "    m:      M;\n"
  "    blob:   72;\n"
  "    n:      8;\n"
  "    d[4]:   valof(n) * 8;\n"
  "    its[2]: It[valof(n)];\n"
  "  };\n"
  "};\n";

static int us23_tc06_an_absent_field_reads_as_absent_from_every_getter(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, GETTERS)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  uint64_t value = 0;
  double eng = 0;
  const char *name = NULL;
  size_t length = 99;
  size_t count = 0;
  uint8_t buffer[16];
  FramedslStatus status = FRAMEDSL_OK;

  EXPECT(decodes_as(&fixture, "60 00", "G"));
  EXPECT(present(&fixture, "G.id") && present(&fixture, "G.on"));
  EXPECT(get_status(&fixture, "G.s") == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "G.c"), &eng) == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_get_name(fixture.frame, id_of(&fixture, "G.m"), &name) == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.blob"), &length, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_ABSENT);
  EXPECT(
    framedsl_get_bytes_into(fixture.frame, id_of(&fixture, "G.blob"), buffer, sizeof(buffer), &length)
    == FRAMEDSL_ERROR_ABSENT
  );
  EXPECT(framedsl_byte_length(fixture.frame, id_of(&fixture, "G.blob")) == 0);
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.d"), &length, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_count(fixture.frame, id_of(&fixture, "G.its"), &count) == FRAMEDSL_ERROR_ABSENT);
  EXPECT(get_status(&fixture, "G.its[0].v") == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.its"), &length, &status) == NULL);
  EXPECT(!present(&fixture, "G.s") && !present(&fixture, "G.its") && !present(&fixture, "G.its[1].v"));

  /* The wrong kind of getter is still the wrong kind: FIELD, before anything is asked about presence. */
  EXPECT(framedsl_get_u64(fixture.frame, id_of(&fixture, "G.blob"), &value) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.s"), &length, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_get_name(fixture.frame, id_of(&fixture, "G.s"), &name) == FRAMEDSL_ERROR_ABSENT);
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "G.on"), &eng) == FRAMEDSL_ERROR_FIELD);

  /* And present, every one of them reads. */
  EXPECT(decodes_as(&fixture, "60 01 05 06 01 111111111111111111 01 DD EE", "G"));
  EXPECT(get(&fixture, "G.s") == 5);
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "G.c"), &eng) == FRAMEDSL_OK && eng == 3.0);
  EXPECT(framedsl_get_name(fixture.frame, id_of(&fixture, "G.m"), &name) == FRAMEDSL_OK && strcmp(name, "B") == 0);
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.blob"), &length, &status) != NULL && length == 9);
  EXPECT(framedsl_count(fixture.frame, id_of(&fixture, "G.its"), &count) == FRAMEDSL_OK && count == 1);
  EXPECT(get(&fixture, "G.its[0].v") == 0xEE);

  /* Bad ids are bad ids, conditional or not. */
  bool here = false;
  FramedslFieldId bogus = { .owner = 0xDEAD, .packet = 0, .field = 0 };
  EXPECT(framedsl_present(fixture.frame, bogus, &here) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_present(NULL, id_of(&fixture, "G.s"), &here) == FRAMEDSL_ERROR_MEMORY);
  EXPECT(framedsl_present(fixture.frame, id_of(&fixture, "G.s"), NULL) == FRAMEDSL_ERROR_MEMORY);

  fixture_close(&fixture);
  return failure;
}

/* A decode that did not carry a field clears its slot: a caller meeting the condition later reads zero
 * -- what an untouched field reads -- and never the field of an earlier frame. */
static int us23_tc07_a_decode_without_a_field_clears_it(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, GETTERS)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  size_t length = 99;
  size_t count = 99;
  FramedslStatus status = FRAMEDSL_OK;

  EXPECT(decodes_as(&fixture, "60 01 05 06 01 111111111111111111 01 DD EE", "G"));
  EXPECT(decodes_as(&fixture, "60 00", "G"));
  EXPECT(set(&fixture, "G.on", 1) == FRAMEDSL_OK);
  EXPECT(
    get(&fixture, "G.s") == 0 && get(&fixture, "G.c") == 0 && get(&fixture, "G.m") == 0 && get(&fixture, "G.n") == 0
  );
  const uint8_t *blob = framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.blob"), &length, &status);
  EXPECT(blob != NULL && length == 9);
  for (size_t i = 0; blob != NULL && i < length; ++i) {
    EXPECT(blob[i] == 0);
  }
  EXPECT(framedsl_get_bytes(fixture.frame, id_of(&fixture, "G.d"), &length, &status) != NULL && length == 0);
  EXPECT(framedsl_count(fixture.frame, id_of(&fixture, "G.its"), &count) == FRAMEDSL_OK && count == 0);
  EXPECT(get(&fixture, "G.its[0].v") == 0);
  /* Re-encoded, it is the empty part it now holds. */
  EXPECT(encodes(&fixture, "G", "60 01 00 00 00 000000000000000000 00"));
  fixture_close(&fixture);
  return failure;
}

/* Setting a field does not depend on whether it is present: the value waits in its slot. */
static int us23_tc08_an_absent_field_can_be_set_in_any_order(const TestArguments *args) {
  (void)args;
  Fixture first;
  Fixture second;
  bool opened = fixture_open(&first, BASIC);
  opened = fixture_open(&second, BASIC) && opened;
  if (!opened) {
    fixture_close(&first);
    fixture_close(&second);
    FORCE_FAILURE();
  }
  int failure = 0;

  /* The field before its condition... */
  EXPECT(set(&first, "P.ext", 0x1234) == FRAMEDSL_OK);
  EXPECT(get_status(&first, "P.ext") == FRAMEDSL_ERROR_ABSENT);
  EXPECT(set(&first, "P.tail", 0xAB) == FRAMEDSL_OK);
  EXPECT(encodes(&first, "P", "10 00 AB"));
  EXPECT(set(&first, "P.flag", 1) == FRAMEDSL_OK);
  EXPECT(get(&first, "P.ext") == 0x1234);
  /* ...and after it: one frame. */
  EXPECT(set(&second, "P.flag", 1) == FRAMEDSL_OK);
  EXPECT(set(&second, "P.ext", 0x1234) == FRAMEDSL_OK);
  EXPECT(set(&second, "P.tail", 0xAB) == FRAMEDSL_OK);
  EXPECT(encodes(&first, "P", "10 01 12 34 AB") && encodes(&second, "P", "10 01 12 34 AB"));

  /* Out of range is still out of range, present or not. */
  EXPECT(set(&first, "P.flag", 0) == FRAMEDSL_OK);
  EXPECT(set(&first, "P.ext", 0x10000) == FRAMEDSL_ERROR_RANGE);

  fixture_close(&first);
  fixture_close(&second);

  /* Every setter writes an absent field. */
  Fixture fixture;
  if (!fixture_open(&fixture, GETTERS)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  const uint8_t blob[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  const uint8_t d[2] = { 0xD1, 0xD2 };
  FramedslFieldId element;
  EXPECT(framedsl_set_eng(fixture.frame, id_of(&fixture, "G.c"), 4.0) == FRAMEDSL_OK);
  EXPECT(framedsl_set_name(fixture.frame, id_of(&fixture, "G.m"), "B") == FRAMEDSL_OK);
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "G.blob"), blob, sizeof(blob)) == FRAMEDSL_OK);
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "G.d"), d, sizeof(d)) == FRAMEDSL_OK);
  EXPECT(framedsl_element(fixture.definition, id_of(&fixture, "G.its.v"), 1, &element) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, element, 0x99) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "G.s", 0x55) == FRAMEDSL_OK && set(&fixture, "G.n", 2) == FRAMEDSL_OK);
  /* Absent, a payload whose length disagrees with its count is not on the wire to disagree. */
  EXPECT(set(&fixture, "G.n", 3) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "G", "60 00"));
  EXPECT(set(&fixture, "G.on", 1) == FRAMEDSL_OK);
  size_t length = 0;
  FramedslStatus status = FRAMEDSL_OK;
  EXPECT(framedsl_encode(fixture.frame, "G", &length, &status) == NULL && status == FRAMEDSL_ERROR_RANGE);
  EXPECT(set(&fixture, "G.n", 2) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "G", "60 01 55 08 01 010203040506070809 02 D1 D2 00 99"));
  fixture_close(&fixture);
  return failure;
}

static int us23_tc09_presence_follows_the_frames_values(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, BASIC)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  EXPECT(!present(&fixture, "P.ext") && present(&fixture, "P.tail"));
  EXPECT(set(&fixture, "P.flag", 1) == FRAMEDSL_OK && present(&fixture, "P.ext"));
  framedsl_frame_reset(fixture.frame);
  EXPECT(!present(&fixture, "P.ext"));
  EXPECT(decodes_as(&fixture, "10 01 00 00 00", "P") && present(&fixture, "P.ext"));
  EXPECT(set(&fixture, "P.flag", 3) == FRAMEDSL_OK && !present(&fixture, "P.ext"));
  fixture_close(&fixture);
  return failure;
}

/* A condition, or a width, reading a fixed field reads its fixed value on a frame being built. A shared
 * header whose optional part depends on the packet's type, fixed by an override, is the case. A decoded
 * frame's fixed fields read what arrived: see story 27. */
static int us23_tc10_a_fixed_field_is_compared_as_its_fixed_value(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "block H { type: 8; [ @if: valof(type) == 0x41; ] ext: 8; };\n"
        "block It { v: 8; };\n"
        "pkt F1 { h: H { type = 0x41; }; t: 8; };\n"
        "pkt F2 { h: H { type = 0x42; }; t: 8; };\n"
        "pkt F3 { k: 8 = 0x43; [ @if: valof(k) == 0x43; ] e: 8; };\n"
        "pkt F4 { n: 8 = 0x02; d[4]: valof(n) * 8; c: 8 = 0x03; its[3]: It[valof(c)]; };\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  const uint8_t two[] = { 0xD1, 0xD2 };
  size_t count = 0;

  EXPECT(present(&fixture, "F1.h.ext") && !present(&fixture, "F2.h.ext") && present(&fixture, "F3.e"));
  EXPECT(set(&fixture, "F1.h.ext", 0x77) == FRAMEDSL_OK && set(&fixture, "F1.t", 0x01) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "F1", "41 77 01"));
  EXPECT(set(&fixture, "F2.h.ext", 0x77) == FRAMEDSL_OK && set(&fixture, "F2.t", 0x01) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "F2", "42 01"));
  EXPECT(set(&fixture, "F3.e", 0x09) == FRAMEDSL_OK && encodes(&fixture, "F3", "43 09"));
  /* The fixed field cannot be written (#140), and what depends on it stands. */
  EXPECT(set(&fixture, "F3.k", 0) == FRAMEDSL_ERROR_FIXED && present(&fixture, "F3.e"));
  EXPECT(encodes(&fixture, "F3", "43 09"));

  /* A width and a count read from a fixed field. */
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "F4.d"), two, sizeof(two)) == FRAMEDSL_OK);
  EXPECT(framedsl_count(fixture.frame, id_of(&fixture, "F4.its"), &count) == FRAMEDSL_OK && count == 3);
  EXPECT(encodes(&fixture, "F4", "02 D1 D2 03 00 00 00"));

  EXPECT(decodes_as(&fixture, "41 66 02", "F1") && get(&fixture, "F1.h.ext") == 0x66);
  EXPECT(decodes_as(&fixture, "42 02", "F2") && get(&fixture, "F2.t") == 2);
  EXPECT(decodes_as(&fixture, "02 AA BB 03 01 02 03", "F4") && get(&fixture, "F4.its[2].v") == 3);
  fixture_close(&fixture);
  return failure;
}

/* `valof(hdr.flag)`: a field of an instance, named through it, in a condition, a width and a count. */
static int us23_tc11_valof_names_a_field_of_an_instance(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "block Len { len: 8; flag: 8; };\n"
        "block It { v: 8; };\n"
        "block Outer {\n"
        "  inner: Len;\n"
        "  [ @if: valof(inner.flag) == 1; ] x: 8;\n"
        "  d[4]:  valof(inner.len) * 8;\n"
        "};\n"
        "pkt D {\n"
        "  id:      8 = 0x70;\n"
        "  hdr:     Len;\n"
        "  body[8]: valof(hdr.len) * 8;\n"
        "  [ @if: valof(hdr.flag) == 1; ] opt: 8;\n"
        "  o:       Outer;\n"
        "  cnt[4]:  It[valof(o.inner.len)];\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  const uint8_t body[] = { 0xAA, 0xBB };
  const uint8_t d[] = { 0xDD };
  EXPECT(set(&fixture, "D.hdr.len", 2) == FRAMEDSL_OK && set(&fixture, "D.hdr.flag", 1) == FRAMEDSL_OK);
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "D.body"), body, sizeof(body)) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "D.opt", 0xCC) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "D.o.inner.len", 1) == FRAMEDSL_OK && set(&fixture, "D.o.x", 0x99) == FRAMEDSL_OK);
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "D.o.d"), d, sizeof(d)) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "D.cnt[0].v", 0xEE) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "D", "70 02 01 AA BB CC 01 00 DD EE"));
  EXPECT(set(&fixture, "D.o.inner.flag", 1) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "D", "70 02 01 AA BB CC 01 01 99 DD EE"));

  framedsl_frame_reset(fixture.frame);
  EXPECT(decodes_as(&fixture, "70 01 00 AA 02 01 77 D1 D2 E1 E2", "D"));
  EXPECT(!present(&fixture, "D.opt") && get(&fixture, "D.o.x") == 0x77 && get(&fixture, "D.cnt[1].v") == 0xE2);
  fixture_close(&fixture);
  return failure;
}

/* Two instances of one block each read their own fields, and a pasted one the fields it pasted in. A
 * block is checked on its own, so what its conditions read is its own, as a width's valof() is. */
static int us23_tc12_each_instance_has_its_own_conditions(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "block Opt { f: 8; [ @if: valof(f) == 1; ] v: 8; };\n"
        "block Tail { mode: 8; [ @if: valof(mode) == 2; ] t: 8; };\n"
        "pkt T2 { id: 8 = 0x71; a: Opt; b: Opt; Tail; };\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  EXPECT(set(&fixture, "T2.a.v", 0xAA) == FRAMEDSL_OK && set(&fixture, "T2.b.v", 0xBB) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "T2.t", 0xCC) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "T2.a.f", 1) == FRAMEDSL_OK && encodes(&fixture, "T2", "71 01 AA 00 00"));
  EXPECT(set(&fixture, "T2.a.f", 0) == FRAMEDSL_OK && set(&fixture, "T2.b.f", 1) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "T2", "71 00 01 BB 00"));
  EXPECT(set(&fixture, "T2.mode", 2) == FRAMEDSL_OK && encodes(&fixture, "T2", "71 00 01 BB 02 CC"));

  size_t packet = 0;
  size_t index = 0;
  FramedslField field;
  EXPECT(framedsl_find_packet(fixture.definition, "T2", &packet));
  EXPECT(
    framedsl_find_field(fixture.definition, packet, "b.v", &index)
    && framedsl_field(fixture.definition, packet, index, &field)
  );
  EXPECT(field.condition != NULL && strcmp(field.condition, "valof(b.f) == 1") == 0);
  EXPECT(
    framedsl_find_field(fixture.definition, packet, "t", &index)
    && framedsl_field(fixture.definition, packet, index, &field)
  );
  EXPECT(field.condition != NULL && strcmp(field.condition, "valof(mode) == 2") == 0);

  /* A block's condition reading a field outside it is the block's mistake, whatever uses it. */
  EXPECT(refused(
    "block Tail { [ @if: valof(mode) == 2; ] t: 8; };\npkt X { id: 8 = 1; mode: 8; Tail; };\n",
    7,
    "names no field in 'Tail'"
  ));
  fixture_close(&fixture);
  return failure;
}

static int us23_tc13_an_imported_block_keeps_its_conditions(const TestArguments *args) {
  int failure = 0;
  EXPECT(write_file(
    args,
    "lib.fdsl",
    "enum Kind: 8 { NONE; EXT; };\n"
    "block Hdr { kind: Kind; [ @if: valof(kind) == EXT; ] ext: 8; };\n"
  ));
  EXPECT(write_file(
    args,
    "flat.fdsl",
    "import \"lib.fdsl\";\n"
    "pkt A { id: 8 = 0x01; h: Hdr; [ @if: valof(h.kind) != NONE; ] more: 8; };\n"
  ));
  EXPECT(write_file(
    args,
    "named.fdsl",
    "import \"lib.fdsl\" as v;\n"
    "pkt B { id: 8 = 0x02; h: v.Hdr; [ @if: valof(h.kind) == EXT; ] more: 8; };\n"
  ));
  const char *const files[] = { "flat.fdsl", "named.fdsl" };
  const char *const packets[] = { "A", "B" };
  const char *const bytes[] = { "01 01 05 06", "02 01 05 06" };
  for (size_t i = 0; i < 2 && failure == 0; ++i) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, files[i]);
    Fixture fixture;
    fixture.result = framedsl_load_file(path);
    fixture.definition = framedsl_definition(fixture.result);
    fixture.frame = fixture.definition != NULL ? framedsl_frame_new(fixture.definition) : NULL;
    char name[64];
    EXPECT(fixture.frame != NULL);
    if (fixture.frame != NULL) {
      snprintf(name, sizeof(name), "%s.h.kind", packets[i]);
      EXPECT(framedsl_set_name(fixture.frame, id_of(&fixture, name), "EXT") == FRAMEDSL_OK);
      snprintf(name, sizeof(name), "%s.h.ext", packets[i]);
      EXPECT(set(&fixture, name, 5) == FRAMEDSL_OK);
      snprintf(name, sizeof(name), "%s.more", packets[i]);
      EXPECT(set(&fixture, name, 6) == FRAMEDSL_OK);
      EXPECT(encodes(&fixture, packets[i], bytes[i]));
      snprintf(name, sizeof(name), "%s.h.kind", packets[i]);
      EXPECT(set(&fixture, name, 0) == FRAMEDSL_OK);
      snprintf(name, sizeof(name), "%s.h.ext", packets[i]);
      EXPECT(!present(&fixture, name));
    }
    fixture_close(&fixture);
  }
  return failure;
}

/* A decode that gives up part-way puts every field back, presence included. */
static int us23_tc14_a_failed_decode_changes_nothing(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, BASIC)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  FramedslStatus status = FRAMEDSL_OK;
  EXPECT(decodes_as(&fixture, "10 01 12 34 AB", "P"));
  EXPECT(decodes(&fixture, "10 00", NULL, &status) == NULL && status == FRAMEDSL_ERROR_TRUNCATED);
  EXPECT(get(&fixture, "P.flag") == 1 && get(&fixture, "P.ext") == 0x1234 && get(&fixture, "P.tail") == 0xAB);
  EXPECT(decodes_as(&fixture, "10 00 CD", "P"));
  EXPECT(decodes(&fixture, "10 01 12 34", NULL, &status) == NULL && status == FRAMEDSL_ERROR_TRUNCATED);
  EXPECT(!present(&fixture, "P.ext") && get(&fixture, "P.tail") == 0xCD);
  fixture_close(&fixture);
  return failure;
}

static int us23_tc15_nothing_allocates(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SHAPES)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  const uint8_t frames[][6] = {
    { 0x20, 0x00, 0x00, 0xEE },
    { 0x20, 0x01, 0xA1, 0xB1, 0x00, 0xEE },
    { 0x20, 0x04, 0x02, 0x11, 0x22, 0xEE },
  };
  const size_t sizes[] = { 4, 6, 6 };
  FramedslFieldId ids[] = { id_of(&fixture, "S.named.a"), id_of(&fixture, "S.items[1].v"), id_of(&fixture, "S.c") };
  FramedslFieldId m = id_of(&fixture, "S.m");

  framedsl_allocation_count_start();
  for (int round = 0; round < 64; ++round) {
    for (size_t i = 0; i < 3; ++i) {
      framedsl_decode(fixture.frame, frames[i], sizes[i], NULL, NULL);
      for (size_t f = 0; f < 3; ++f) {
        bool here = false;
        uint64_t value = 0;
        framedsl_present(fixture.frame, ids[f], &here);
        framedsl_get_u64(fixture.frame, ids[f], &value);
      }
      size_t length = 0;
      framedsl_set_u64(fixture.frame, m, (uint64_t)round % 5);
      framedsl_encode(fixture.frame, "S", &length, NULL);
    }
  }
  EXPECT(framedsl_allocation_count_stop() == 0);
  fixture_close(&fixture);
  return failure;
}

/* A capture of frames with and without their conditional parts, back to back. */
static int us23_tc16_a_stream_is_walked_by_what_each_frame_carried(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, BASIC)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  uint8_t stream[64];
  size_t length = parse_hex("10 00 AB  10 01 12 34 AB  10 00 CD  10 01 00 01 EF", stream, sizeof(stream));
  const size_t sizes[] = { 3, 5, 3, 5 };
  size_t offset = 0;
  for (size_t i = 0; i < 4; ++i) {
    size_t consumed = 0;
    EXPECT(framedsl_decode(fixture.frame, stream + offset, length - offset, &consumed, NULL) != NULL);
    EXPECT(consumed == sizes[i]);
    offset += consumed;
  }
  EXPECT(offset == length && get(&fixture, "P.ext") == 1 && get(&fixture, "P.tail") == 0xEF);
  fixture_close(&fixture);
  return failure;
}

void register_user_story23(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us23_tc01_a_conditional_field_is_on_the_wire_only_when_it_holds, "on the wire only when it holds", false
  );
  ADD_TEST_CASE(
    us23_tc02_every_kind_of_declaration_can_be_conditional, "every kind of declaration can be conditional", false
  );
  ADD_TEST_CASE(us23_tc03_nested_conditions_all_have_to_hold, "nested conditions all have to hold", false);
  ADD_TEST_CASE(us23_tc04_an_enum_field_is_compared_by_name, "an enum field is compared by name", false);
  ADD_TEST_CASE(
    us23_tc05_fields_after_a_conditional_part_are_found_in_both_cases, "what follows is found in both cases", false
  );
  ADD_TEST_CASE(us23_tc06_an_absent_field_reads_as_absent_from_every_getter, "absent from every getter", false);
  ADD_TEST_CASE(us23_tc07_a_decode_without_a_field_clears_it, "a decode without a field clears it", false);
  ADD_TEST_CASE(us23_tc08_an_absent_field_can_be_set_in_any_order, "an absent field can be set, in any order", false);
  ADD_TEST_CASE(us23_tc09_presence_follows_the_frames_values, "presence follows the frame's values", false);
  ADD_TEST_CASE(
    us23_tc10_a_fixed_field_is_compared_as_its_fixed_value, "a fixed field is compared as its fixed value", false
  );
  ADD_TEST_CASE(us23_tc11_valof_names_a_field_of_an_instance, "valof() names a field of an instance", false);
  ADD_TEST_CASE(us23_tc12_each_instance_has_its_own_conditions, "each instance has its own conditions", false);
  ADD_TEST_CASE(us23_tc13_an_imported_block_keeps_its_conditions, "an imported block keeps its conditions", false);
  ADD_TEST_CASE(us23_tc14_a_failed_decode_changes_nothing, "a failed decode changes nothing", false);
  ADD_TEST_CASE(us23_tc15_nothing_allocates, "nothing allocates", false);
  ADD_TEST_CASE(
    us23_tc16_a_stream_is_walked_by_what_each_frame_carried, "a stream is walked by what each frame carried", false
  );
}
