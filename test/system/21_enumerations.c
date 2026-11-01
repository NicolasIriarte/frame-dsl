/* Story 21: enumerations (#11) -- `enum Name: width { A; B = 5; C; };`, and fields typed by one.
 *
 * Through the public API and the binary, as a program and a person at a terminal meet them: a field
 * typed by an enum is read and written by name or by number, its fixed values and overrides are names,
 * a value the enum does not list decodes and is flagged, and the file is refused when an enum or a use
 * of one is wrong -- each for its own check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool fixture_open(Fixture *fixture, const char *source) {
  fixture->result = framedsl_load_string(source, "enums.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *name) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, name, &id);
  return id;
}

static bool encodes_to(Fixture *fixture, const char *packet, const uint8_t *want, size_t want_length) {
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, NULL);
  return bytes != NULL && length == want_length && memcmp(bytes, want, length) == 0;
}

/* The diagnostic `source` is refused with for check `wanted`, or NULL when the load succeeded or was
 * refused only for others. Valid until `*result` is freed. */
static const FramedslDiagnostic *refusal(const char *source, int wanted, FramedslResult **result) {
  *result = framedsl_load_string(source, "enums.fdsl");
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

static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "enums.fdsl");
  bool ok = framedsl_ok(result);
  framedsl_result_free(result);
  return ok;
}

static int run_capture(const char *command, char *output, size_t capacity) {
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return -1;
  }
  size_t used = fread(output, 1, capacity - 1, pipe);
  output[used] = '\0';
  int closed = pclose(pipe);
  return WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
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

static const char *const ANTENNA =
  "enum PowerState: 1 {\n"
  "  OFF;\n"
  "  ON;\n"
  "};\n"
  "enum TxLevel: 3 {\n"
  "  OFF;\n"
  "  LOW;\n"
  "  MID;\n"
  "  HIGH;\n"
  "  MAX = 7;\n"
  "};\n"
  "pkt TM {\n"
  "  id:    4 := 0x2;\n"
  "  power: PowerState;\n"
  "  level: TxLevel;\n"
  "};\n";

/* ---- Reading and writing ----------------------------------------------------------------------- */

static int us21_tc01_a_field_reads_and_writes_by_name_and_by_number(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, ANTENNA)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  FramedslFieldId power = id_of(&fixture, "TM.power");
  FramedslFieldId level = id_of(&fixture, "TM.level");
  const uint8_t high[] = { 0x2B }; /* 0010 1 011 */
  const uint8_t max[] = { 0x2F }; /* 0010 1 111 */
  const char *name = NULL;
  uint64_t value = 0;
  FramedslField described;
  int failure = 0;

  if (framedsl_set_name(fixture.frame, power, "ON") != FRAMEDSL_OK
      || framedsl_set_name(fixture.frame, level, "HIGH") != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, level, &value) != FRAMEDSL_OK || value != 3) {
    failure = __LINE__; /* the arena holds the number */
  } else if (!encodes_to(&fixture, "TM", high, sizeof(high))) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, level, 7) != FRAMEDSL_OK
             || framedsl_get_name(fixture.frame, level, &name) != FRAMEDSL_OK || strcmp(name, "MAX") != 0) {
    failure = __LINE__; /* written by number, read by name */
  } else if (!encodes_to(&fixture, "TM", max, sizeof(max))) {
    failure = __LINE__;
  } else if (!framedsl_field(fixture.definition, 0, 2, &described) || described.enumeration == NULL
             || strcmp(described.enumeration, "TxLevel") != 0 || described.width_bits != 3) {
    failure = __LINE__; /* the enum decides the width, and introspection says which enum */
  } else if (!framedsl_field(fixture.definition, 0, 0, &described) || described.enumeration != NULL) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

/* A name without a value is one more than the name before it, and the first is 0. */
static int us21_tc02_a_name_without_a_value_counts_on(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  const char *source =
    "enum TxLevel: 3 { OFF; LOW; MID; HIGH; MAX = 7; };\n"
    "enum Mode: 16 { IDLE = 0x0100; RECEIVE; TRANSMIT; };\n"
    "pkt P { id: 5 := 1; level: TxLevel; mode: Mode; };\n";
  if (!fixture_open(&fixture, source)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  const struct {
    const char *enumeration;
    const char *names[5];
    uint64_t values[5];
    size_t count;
  } want[] = {
    { "TxLevel", { "OFF", "LOW", "MID", "HIGH", "MAX" }, { 0, 1, 2, 3, 7 },       5 },
    { "Mode",    { "IDLE", "RECEIVE", "TRANSMIT" },      { 0x100, 0x101, 0x102 }, 3 },
  };
  int failure = framedsl_enum_count(fixture.definition) == 2 ? 0 : __LINE__;
  for (size_t e = 0; failure == 0 && e < 2; ++e) {
    size_t index = 99;
    if (!framedsl_find_enum(fixture.definition, want[e].enumeration, &index) || index != e
        || strcmp(framedsl_enum_name(fixture.definition, index), want[e].enumeration) != 0
        || framedsl_enum_item_count(fixture.definition, index) != want[e].count) {
      failure = __LINE__;
    }
    for (size_t i = 0; failure == 0 && i < want[e].count; ++i) {
      const char *name = NULL;
      uint64_t value = 0;
      if (!framedsl_enum_item(fixture.definition, index, i, &name, &value) || strcmp(name, want[e].names[i]) != 0
          || value != want[e].values[i]) {
        failure = __LINE__;
      }
    }
  }
  /* Past the end is refused rather than read. */
  if (failure == 0
      && (framedsl_enum_item(fixture.definition, 0, 5, NULL, NULL) || framedsl_enum_item_count(fixture.definition, 2) != 0
          || framedsl_enum_name(fixture.definition, 2) != NULL || framedsl_find_enum(fixture.definition, "Nope", NULL))) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

/* ---- The enum itself ----------------------------------------------------------------------------- */

static int us21_tc03_names_are_unique_and_values_increase(const TestArguments *args) {
  (void)args;
  if (!refused("enum E: 4 { A; B; A; };\npkt P { id: 4 := 1; e: E; };\n", 36, "already has a name 'A'")) {
    return __LINE__;
  }
  /* Two names for one value, written or counted, and a value lower than the one before. */
  if (!refused("enum E: 4 { A; B; C = 1; };\npkt P { id: 4 := 1; e: E; };\n", 37, "does not come after 'B'")) {
    return __LINE__;
  }
  if (!refused("enum E: 4 { A = 5; B; C = 2; };\npkt P { id: 4 := 1; e: E; };\n", 37, "'C' is 2")) {
    return __LINE__;
  }
  if (!loads("enum E: 4 { A = 2; B = 9; C; };\npkt P { id: 4 := 1; e: E; };\n")) {
    return __LINE__; /* gaps are fine */
  }
  return 0;
}

static int us21_tc04_every_value_fits_and_the_width_is_a_scalars(const TestArguments *args) {
  (void)args;
  if (!refused("enum E: 2 { A; B; C; D; X = 4; };\npkt P { id: 6 := 1; e: E; };\n", 38, "'X' is 4")) {
    return __LINE__; /* written */
  }
  if (!refused("enum E: 2 { A = 3; B; };\npkt P { id: 6 := 1; e: E; };\n", 38, "'B' is 4")) {
    return __LINE__; /* counted */
  }
  if (!refused(
        "enum E: 64 { A = 0xFFFFFFFFFFFFFFFF; B; };\npkt P { e: E := A; };\n", 38, "counts past the largest 64-bit"
      )) {
    return __LINE__; /* counted past anything a field can hold */
  }
  if (!loads("enum E: 64 { A = 0xFFFFFFFFFFFFFFFE; B; };\npkt P { e: E := B; };\n")) {
    return __LINE__; /* the largest value is a value */
  }
  if (!refused("enum E: 0 { A; };\npkt P { id: 8 := 1; };\n", 35, "1 to 64 bits")
      || !refused("enum E: 65 { A; };\npkt P { id: 8 := 1; };\n", 35, "65 bits")) {
    return __LINE__;
  }
  if (!refused("enum E: 4 { };\npkt P { id: 8 := 1; };\n", 0, "at least one value")) {
    return __LINE__;
  }
  return 0;
}

/* ---- Values written as names --------------------------------------------------------------------- */

static const char *const FAMILY =
  "enum PktType: 4 { HK = 0x1; EVENT; };\n"
  "block Header { type: PktType; seq: 12; };\n"
  "pkt A { hdr: Header { type := HK; }; x: 8; };\n"
  "pkt B { hdr: Header { type := %s; }; y: 8; };\n"
  "pkt C { kind: PktType := %s; pad: 4; z: 16; };\n";

static int us21_tc05_a_name_is_a_fixed_value_and_identifies_a_packet(const TestArguments *args) {
  (void)args;
  char source[1024];
  snprintf(source, sizeof(source), FAMILY, "EVENT", "EVENT");
  /* C fixes the same four bits as B; y and z differ in width, which does not separate them. */
  if (!refused(source, 12, "'B' and 'C'")) {
    return __LINE__;
  }
  /* The same name as A's, through a block override and a field of its own: also indistinguishable. */
  snprintf(source, sizeof(source), FAMILY, "HK", "EVENT");
  if (!refused(source, 12, "'A' and 'B'")) {
    return __LINE__;
  }

  Fixture fixture;
  const char *family =
    "enum PktType: 4 { HK = 0x1; EVENT; STATUS = 0x8; };\n"
    "block Header { type: PktType; seq: 12; };\n"
    "pkt A { hdr: Header { type := HK; }; x: 8; };\n"
    "pkt B { hdr: Header { type := EVENT; }; y: 8; };\n"
    "pkt C { kind: PktType := STATUS; pad: 4; z: 16; };\n";
  if (!fixture_open(&fixture, family)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  int failure = 0;
  const uint8_t a[] = { 0x10, 0x05, 0x09 };
  const uint8_t b[] = { 0x20, 0x00, 0x07 };
  const uint8_t c[] = { 0x80, 0x12, 0x34 };
  FramedslField kind;
  if (framedsl_set_u64(fixture.frame, id_of(&fixture, "A.hdr.seq"), 5) != FRAMEDSL_OK
      || framedsl_set_u64(fixture.frame, id_of(&fixture, "A.x"), 9) != FRAMEDSL_OK
      || !encodes_to(&fixture, "A", a, sizeof(a))) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, b, sizeof(b), NULL, NULL) == NULL
             || strcmp(framedsl_decode(fixture.frame, b, sizeof(b), NULL, NULL), "B") != 0) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, c, sizeof(c), NULL, NULL) == NULL
             || strcmp(framedsl_decode(fixture.frame, c, sizeof(c), NULL, NULL), "C") != 0) {
    failure = __LINE__;
  } else if (!framedsl_field(fixture.definition, 2, 0, &kind) || !kind.has_fixed || kind.fixed != 0x8) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

static int us21_tc06_a_value_is_a_name_the_fields_enum_lists(const TestArguments *args) {
  (void)args;
  const char *const HEAD = "enum E: 2 { A; B; };\nenum F: 2 { X; Y; };\nblock H { e: E; n: 6; };\n";
  const struct {
    const char *body;
    const char *text;
  } cases[] = {
    { "pkt P { e: E := 1; pad: 6; };",                         "write 'B' for 1"               },
    { "pkt P { e: E := 3; pad: 6; };",                         "not a number"                  },
    { "pkt P { e: E := C; pad: 6; };",                         "enum 'E' has no name 'C'"      },
    { "pkt P { e: E := X; pad: 6; };",                         "enum 'E' has no name 'X'"      }, /* F's, not E's */
    { "pkt P { n: 8 := A; };",                                 "'n' is not typed by an enum"   },
    { "pkt P { h: H { e := 1; }; };",                          "'h.e' is typed by enum 'E'"    },
    { "pkt P { h: H { n := A; e := A; }; };",                  "'h.n' is not typed by an enum" },
    { "block G { e: E := Z; n: 6; };\npkt P { id: 8 := 1; };", "enum 'E' has no name 'Z'"      },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char source[512];
    snprintf(source, sizeof(source), "%s%s\n", HEAD, cases[i].body);
    if (!refused(source, 39, cases[i].text)) {
      return __LINE__ * 100 + (int)i;
    }
  }

  /* A misspelt name comes with the one it meant. */
  FramedslResult *result = NULL;
  const FramedslDiagnostic *typo =
    refusal("enum E: 2 { ALPHA; BETA; };\npkt P { e: E := BTEA; pad: 6; };\n", 39, &result);
  int failure = 0;
  if (typo == NULL || typo->fix_text == NULL || strcmp(typo->fix_text, "BETA") != 0 || typo->line != 2
      || typo->fix_column != 17) {
    failure = __LINE__;
  }
  framedsl_result_free(result);

  /* The type says which enum, so the name is written bare. */
  if (failure == 0 && !refused("enum E: 2 { A; };\npkt P { e: E := E.A; pad: 6; };\n", 0, "bare name")) {
    failure = __LINE__;
  }
  return failure;
}

/* ---- Names shared, and what an enum field is not ------------------------------------------------- */

static int us21_tc07_an_enum_and_a_block_share_their_names(const TestArguments *args) {
  (void)args;
  if (!refused("block X { a: 8; };\nenum X: 2 { A; };\npkt P { id: 8 := 1; x: X; };\n", 1, "already a block")) {
    return __LINE__;
  }
  if (!refused("enum X: 2 { A; };\nblock X { a: 8; };\npkt P { id: 8 := 1; x: X; };\n", 1, "already an enum")) {
    return __LINE__; /* reported at the later of the two */
  }
  if (!refused("enum X: 2 { A; };\nenum X: 2 { B; };\npkt P { id: 6 := 1; x: X; };\n", 1, "already an enum")) {
    return __LINE__;
  }
  return 0;
}

static int us21_tc08_an_enum_field_takes_no_calibration(const TestArguments *args) {
  (void)args;
  const char *source = "cal twice(raw) = raw * 2;\nenum E: 4 { A; };\npkt P { id: 4 := 1; [ @cal: twice; ] e: E; };\n";
  if (!refused(source, 40, "'P.e' is typed by enum 'E'")) {
    return __LINE__;
  }
  return 0;
}

static int us21_tc09_an_enum_field_is_named_once_and_not_repeated(const TestArguments *args) {
  (void)args;
  const char *const HEAD = "enum E: 8 { A; B; };\n";
  const struct {
    const char *body;
    int check;
    const char *text;
  } cases[] = {
    { "pkt P { id: 8 := 1; E; };",                41, "name the field, 'name: E;'"      },
    { "pkt P { id: 8 := 1; E { x := 1; }; };",    41, "no fields to paste in"           },
    { "pkt P { id: 8 := 1; e: E { x := 1; }; };", 41, "'e: E := NAME;'"                 },
    { "pkt P { id: 8 := 1; e: E[2]; };",          41, "only a block can be repeated"    },
    { "pkt P { id: 8 := 1; e[4]: E; };",          14, NULL                              },
    { "pkt P { id: 8 := 1; e: Ee; };",            23, "which is not a block or an enum" },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char source[512];
    snprintf(source, sizeof(source), "%s%s\n", HEAD, cases[i].body);
    if (!refused(source, cases[i].check, cases[i].text)) {
      return __LINE__ * 100 + (int)i;
    }
  }
  /* Declared above everything that uses it, as a block is. */
  if (!refused("pkt P { id: 8 := 1; e: E; };\nenum E: 8 { A; };\n", 24, "before it is declared")) {
    return __LINE__;
  }
  /* An enum sets no layout, so a group has nothing to say to it. */
  if (!refused("[ @byte_order: little_endian; ]\nenum E: 8 { A; };\npkt P { id: 8 := 1; };\n", 0, "no layout")) {
    return __LINE__;
  }
  return 0;
}

/* The enum decides the width and the names; the field's orders are its own, from where it is used. */
static int us21_tc10_orders_come_from_where_the_field_is_used(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  const char *source =
    "enum Mode: 16 { IDLE = 0x1234; RUN; };\n"
    "[ @byte_order: little_endian; ]\n"
    "pkt P {\n"
    "  id:    8 := 0x40;\n"
    "  mode:  Mode;\n"
    "  [ @byte_order: big_endian; ]\n"
    "  other: Mode;\n"
    "};\n";
  if (!fixture_open(&fixture, source)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  const uint8_t want[] = { 0x40, 0x35, 0x12, 0x12, 0x35 };
  int failure = 0;
  if (framedsl_set_name(fixture.frame, id_of(&fixture, "P.mode"), "RUN") != FRAMEDSL_OK
      || framedsl_set_name(fixture.frame, id_of(&fixture, "P.other"), "RUN") != FRAMEDSL_OK
      || !encodes_to(&fixture, "P", want, sizeof(want))) {
    failure = __LINE__;
  }
  fixture_close(&fixture);

  const char *bits = "enum Nib: 4 { A = 0x1; };\npkt Q { [ @bit_order: lsb_first; ] n: Nib := A; m: Nib; };\n";
  const uint8_t reversed[] = { 0x80 };
  if (failure == 0 && (!fixture_open(&fixture, bits) || !encodes_to(&fixture, "Q", reversed, sizeof(reversed)))) {
    failure = __LINE__;
  }
  fixture_close(&fixture);

  /* Check 10 holds as for any field: a 4-bit field has no bytes to reorder. */
  if (failure == 0
      && !refused("enum Nib: 4 { A; };\npkt R { id: 4 := 1; [ @byte_order: little_endian; ] n: Nib; };\n", 10, NULL)) {
    failure = __LINE__;
  }
  return failure;
}

/* ---- Values the enum does not list --------------------------------------------------------------- */

static int us21_tc11_an_unlisted_value_decodes_and_is_flagged(const TestArguments *args) {
  Fixture fixture;
  if (!fixture_open(&fixture, ANTENNA)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  const uint8_t frame[] = { 0x2D }; /* 0010 1 101: level 5, which TxLevel does not list */
  FramedslStatus status = FRAMEDSL_OK;
  const char *packet = framedsl_decode(fixture.frame, frame, sizeof(frame), NULL, &status);
  const char *name = "untouched";
  uint64_t value = 0;
  int failure = 0;
  if (packet == NULL || strcmp(packet, "TM") != 0) {
    failure = __LINE__; /* the frame arrived, so it decodes */
  } else if (framedsl_get_name(fixture.frame, id_of(&fixture, "TM.level"), &name) != FRAMEDSL_ERROR_UNLISTED
             || name != NULL) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, id_of(&fixture, "TM.level"), &value) != FRAMEDSL_OK || value != 5) {
    failure = __LINE__; /* and the number is there to read */
  } else if (framedsl_set_u64(fixture.frame, id_of(&fixture, "TM.level"), 6) != FRAMEDSL_OK) {
    failure = __LINE__; /* raw stays raw, both ways */
  }
  fixture_close(&fixture);
  if (failure != 0) {
    return failure;
  }

  /* At the terminal, listed and unlisted alike. */
  if (!write_file(args, "antenna.fdsl", ANTENNA)) {
    return __LINE__;
  }
  char command[1024];
  static char output[8192];
  snprintf(command, sizeof(command), "%s %s/antenna.fdsl -d '2D' 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  if (run_capture(command, output, sizeof(output)) != 0 || strstr(output, "5 (0x5) = <unlisted> [TxLevel]") == NULL
      || strstr(output, "1 (0x1) = ON [PowerState]") == NULL) {
    return __LINE__;
  }
  snprintf(command, sizeof(command), "%s %s/antenna.fdsl -d '2B' 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  if (run_capture(command, output, sizeof(output)) != 0 || strstr(output, "3 (0x3) = HIGH [TxLevel]") == NULL) {
    return __LINE__;
  }
  return 0;
}

static int us21_tc12_an_unknown_name_is_refused_without_writing(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, ANTENNA)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  FramedslFieldId level = id_of(&fixture, "TM.level");
  const char *name = NULL;
  uint64_t value = 0;
  int failure = 0;
  if (framedsl_set_name(fixture.frame, level, "MID") != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_set_name(fixture.frame, level, "LOUD") != FRAMEDSL_ERROR_UNLISTED
             || framedsl_set_name(fixture.frame, level, "TxLevel.MAX") != FRAMEDSL_ERROR_UNLISTED
             || framedsl_set_name(fixture.frame, level, NULL) != FRAMEDSL_ERROR_UNLISTED) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, level, &value) != FRAMEDSL_OK || value != 2) {
    failure = __LINE__; /* still MID */
  } else if (framedsl_set_name(fixture.frame, id_of(&fixture, "TM.id"), "ON") != FRAMEDSL_ERROR_FIELD
             || framedsl_get_name(fixture.frame, id_of(&fixture, "TM.id"), &name) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* a field with no enum has no names */
  } else if (framedsl_get_name(NULL, level, &name) != FRAMEDSL_ERROR_MEMORY
             || framedsl_set_name(NULL, level, "MID") != FRAMEDSL_ERROR_MEMORY) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

/* ---- Imports ----------------------------------------------------------------------------------- */

static int us21_tc13_an_imported_enum_is_a_type_flat_or_named(const TestArguments *args) {
  if (!write_file(args, "modes.fdsl", "enum Mode: 2 { RX = 1; TX; };\nblock Radio { mode: Mode; rest: 6; };\n")
      || !write_file(
        args,
        "device.fdsl",
        "import \"modes.fdsl\";\n"
        "import \"modes.fdsl\" as vendor;\n"
        "pkt P {\n"
        "  id:  8 := 1;\n"
        "  a:   Mode := RX;\n"
        "  b:   vendor.Mode := TX;\n"
        "  r:   vendor.Radio { mode := TX; };\n"
        "  pad: 4;\n"
        "};\n"
      )) {
    return __LINE__;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/device.fdsl", args->tmp_dir);
  FramedslResult *result = framedsl_load_file(path);
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  FramedslField field;
  FramedslFieldId rest = { 0 };
  int failure = 0;
  if (frame == NULL || framedsl_diagnostic_count(result) != 0) {
    failure = __LINE__;
  } else if (!framedsl_field(definition, 0, 2, &field) || strcmp(field.enumeration, "vendor.Mode") != 0
             || !field.has_fixed || field.fixed != 2) {
    failure = __LINE__;
  } else if (!framedsl_field(definition, 0, 3, &field) || strcmp(field.name, "r.mode") != 0
             || strcmp(field.enumeration, "vendor.Mode") != 0 || field.fixed != 2) {
    failure = __LINE__; /* the imported block's field keeps its enum, under the import's name */
  } else if (!framedsl_find(definition, "P.r.rest", &rest) || framedsl_set_u64(frame, rest, 0x3F) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    size_t length = 0;
    const uint8_t want[] = { 0x01, 0x6B, 0xF0 }; /* 01 10 | 10 111111 | 0000 */
    const uint8_t *bytes = framedsl_encode(frame, "P", &length, NULL);
    failure = bytes != NULL && length == sizeof(want) && memcmp(bytes, want, length) == 0 ? 0 : __LINE__;
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  if (failure != 0) {
    return failure;
  }

  /* Two different enums of one name, flat: a clash, as for blocks. So is an enum and a block. */
  if (!write_file(args, "other.fdsl", "enum Mode: 2 { OFF; };\n")
      || !write_file(args, "blocky.fdsl", "block Mode { x: 8; };\n")
      || !write_file(args, "clash.fdsl", "import \"modes.fdsl\";\nimport \"other.fdsl\";\npkt P { id: 8 := 1; };\n")
      || !write_file(args, "mixed.fdsl", "import \"modes.fdsl\";\nimport \"blocky.fdsl\";\npkt P { id: 8 := 1; };\n")) {
    return __LINE__;
  }
  const char *clashing[] = { "clash.fdsl", "mixed.fdsl" };
  for (size_t i = 0; i < 2; ++i) {
    snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, clashing[i]);
    result = framedsl_load_file(path);
    bool clash = false;
    for (size_t d = 0; d < framedsl_diagnostic_count(result); ++d) {
      clash |= framedsl_diagnostic(result, d)->check == 28;
    }
    framedsl_result_free(result);
    if (!clash) {
      return __LINE__ * 100 + (int)i;
    }
  }

  /* A block of this file named like an imported enum is check 1, at the block. */
  if (!write_file(args, "local.fdsl", "import \"other.fdsl\";\nblock Mode { x: 8; };\npkt P { m: Mode; };\n")) {
    return __LINE__;
  }
  snprintf(path, sizeof(path), "%s/local.fdsl", args->tmp_dir);
  result = framedsl_load_file(path);
  failure = __LINE__;
  for (size_t d = 0; d < framedsl_diagnostic_count(result); ++d) {
    const FramedslDiagnostic *diagnostic = framedsl_diagnostic(result, d);
    if (diagnostic->check == 1 && diagnostic->line == 2) {
      failure = 0;
    }
  }
  framedsl_result_free(result);
  return failure;
}

/* `enum` means something only where a declaration starts. */
static int us21_tc14_a_field_may_still_be_called_enum(const TestArguments *args) {
  (void)args;
  if (!loads("block B { enum: 8; };\npkt P { enum: 8 = 1; b: B; };\n")) {
    return __LINE__;
  }
  if (!loads("enum enum: 8 { enum; };\npkt P { enum: enum = enum; };\n")) {
    return __LINE__;
  }
  return 0;
}

void register_user_story21(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us21_tc01_a_field_reads_and_writes_by_name_and_by_number, "a field reads and writes by name and by number", false
  );
  ADD_TEST_CASE(us21_tc02_a_name_without_a_value_counts_on, "a name without a value counts on from the last", false);
  ADD_TEST_CASE(us21_tc03_names_are_unique_and_values_increase, "names are unique and values increase", false);
  ADD_TEST_CASE(
    us21_tc04_every_value_fits_and_the_width_is_a_scalars, "every value fits, and the width is a scalar's", false
  );
  ADD_TEST_CASE(
    us21_tc05_a_name_is_a_fixed_value_and_identifies_a_packet, "a name is a fixed value, and identifies a packet", false
  );
  ADD_TEST_CASE(us21_tc06_a_value_is_a_name_the_fields_enum_lists, "a value is a name the field's enum lists", false);
  ADD_TEST_CASE(us21_tc07_an_enum_and_a_block_share_their_names, "an enum and a block share their names", false);
  ADD_TEST_CASE(us21_tc08_an_enum_field_takes_no_calibration, "an enum field takes no calibration", false);
  ADD_TEST_CASE(
    us21_tc09_an_enum_field_is_named_once_and_not_repeated, "an enum field is named, unfixed inside, unrepeated", false
  );
  ADD_TEST_CASE(
    us21_tc10_orders_come_from_where_the_field_is_used, "bit and byte order come from where it is used", false
  );
  ADD_TEST_CASE(us21_tc11_an_unlisted_value_decodes_and_is_flagged, "an unlisted value decodes, and is flagged", false);
  ADD_TEST_CASE(
    us21_tc12_an_unknown_name_is_refused_without_writing, "an unknown name is refused without writing", false
  );
  ADD_TEST_CASE(us21_tc13_an_imported_enum_is_a_type_flat_or_named, "an imported enum is a type, flat or named", false);
  ADD_TEST_CASE(us21_tc14_a_field_may_still_be_called_enum, "a field may still be called enum", false);
}
