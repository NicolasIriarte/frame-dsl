/* Story 24: conditional fields (#12), the rules and the tools -- `[ @if: valof(name) == value; ]` in front of a field,
 * a block's use or an anonymous block.
 *
 * The half of story 23 about what a file may say: a file is refused, each time for its own check, when
 * a condition cannot be decided, could read a field that is not there, or would leave a frame without a
 * byte form or a way to be recognised. And what the tools show of a condition, and a seeded walk of
 * random frames through encode and decode. */
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

/* Whether `source` is refused at all, with exactly one error, mentioning `text`. */
static bool refused_once(const char *source, const char *text) {
  FramedslResult *result = framedsl_load_string(source, "conditions.fdsl");
  size_t errors = 0;
  bool mentioned = false;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diagnostic = framedsl_diagnostic(result, i);
    if (diagnostic->severity == FRAMEDSL_SEVERITY_ERROR) {
      errors += 1;
      mentioned = mentioned || strstr(diagnostic->message, text) != NULL;
    }
  }
  bool ok = !framedsl_ok(result) && errors == 1 && mentioned;
  framedsl_result_free(result);
  return ok;
}

static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "conditions.fdsl");
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

static const char *const SHAPES =
  "block Pair { a: 8; b: 8; };\n"
  "block Item { v: 8; };\n"
  "pkt S {\n"
  "  id: 8 := 0x20;\n"
  "  m:  8;\n"
  "  [ @if: valof(m) == 1; ] named: Pair;\n"
  "  [ @if: valof(m) == 2; ] Pair;\n"
  "  [ @if: valof(m) == 3; ] block { c: 8; d: 8; };\n"
  "  n:  8;\n"
  "  [ @if: valof(m) == 4; ] items[4]: Item[valof(n)];\n"
  "  end: 8;\n"
  "};\n";

/* `@if` shares a group with the other attributes, in any order, and a group's orders apply inside. */
static int us24_tc01_a_condition_shares_its_group(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "cal twice(raw) = raw * 2;\n"
        "pkt L {\n"
        "  id: 8 := 0x80;\n"
        "  f:  8;\n"
        "  [ @byte_order: little_endian; @if: valof(f) == 1; ] le: 16;\n"
        "  [ @if: valof(f) == 1; @bit_order: lsb_first; @cal: twice; ] bits: 8;\n"
        "  [\n"
        "    @if:         valof(f) == 1;\n"
        "    @byte_order: little_endian;\n"
        "  ]\n"
        "  block {\n"
        "    w: 32;\n"
        "    [ @byte_order: big_endian; ] be: 16;\n"
        "  };\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  double eng = 0;
  EXPECT(set(&fixture, "L.f", 1) == FRAMEDSL_OK && set(&fixture, "L.le", 0x1234) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "L.bits", 0x01) == FRAMEDSL_OK && set(&fixture, "L.w", 0x11223344) == FRAMEDSL_OK);
  EXPECT(set(&fixture, "L.be", 0x5566) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "L", "80 01 34 12 80 44 33 22 11 55 66"));
  EXPECT(framedsl_get_eng(fixture.frame, id_of(&fixture, "L.bits"), &eng) == FRAMEDSL_OK && eng == 2.0);
  EXPECT(set(&fixture, "L.f", 0) == FRAMEDSL_OK && encodes(&fixture, "L", "80 00"));
  fixture_close(&fixture);
  return failure;
}

/* ---- What a file may say ------------------------------------------------------------------------ */

/* Check 42: a valof() reads a conditional field only from under its conditions, or stricter ones. */
static int us24_tc02_a_valof_reads_only_a_field_that_is_there(const TestArguments *args) {
  (void)args;
  int failure = 0;
  /* A width outside the condition. */
  EXPECT(
    refused("pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] len: 8; d[4]: valof(len) * 8; };\n", 42, "valof(h) == 1")
  );
  /* A count outside it. */
  EXPECT(refused(
    "block I { v: 8; };\npkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] n: 8; is[4]: I[valof(n)]; };\n", 42, NULL
  ));
  /* A condition outside it, and one under a different condition. */
  EXPECT(
    refused("pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] k: 8; [ @if: valof(k) == 2; ] x: 8; };\n", 42, NULL)
  );
  EXPECT(refused(
    "pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] k: 8; [ @if: valof(h) == 2; ] block { [ @if: valof(k) == 2; ] "
    "x: "
    "8; }; };\n",
    42,
    NULL
  ));
  /* `!=` is a different condition from `==`, whatever the value. */
  EXPECT(refused(
    "pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] len: 8; [ @if: valof(h) != 1; ] d[4]: valof(len) * 8; };\n",
    42,
    NULL
  ));
  /* Under the same condition -- in one group, in another group saying the same, and nested deeper. */
  EXPECT(loads("pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] block { len: 8; d[4]: valof(len) * 8; }; };\n"));
  EXPECT(loads(
    "pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] len: 8; [ @if: valof(h) == 1; ] d[4]: valof(len) * 8; };\n"
  ));
  EXPECT(loads(
    "pkt T { id: 8 := 1; h: 8; g: 8; [ @if: valof(h) == 1; ] len: 8;\n"
    "  [ @if: valof(g) == 0; ] block { [ @if: valof(h) == 1; ] d[4]: valof(len) * 8; }; };\n"
  ));
  /* A nested condition reading a field of the part it is in. */
  EXPECT(
    loads("pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] block { k: 8; [ @if: valof(k) == 2; ] x: 8; }; };\n")
  );
  /* An unconditional field is readable from anywhere. */
  EXPECT(loads("pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] block { [ @if: valof(id) == 1; ] x: 8; }; };\n"));
  return failure;
}

/* Check 13: a conditional part is whole bytes on its own. */
static int us24_tc03_a_conditional_part_is_whole_bytes(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt T { id: 8 := 1; f: 4; [ @if: valof(f) == 1; ] e: 4; p: 4; };\n", 13, "4 bit(s)"));
  EXPECT(
    refused("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] e: 12; [ @if: valof(f) == 2; ] g: 4; };\n", 13, NULL)
  );
  /* Sub-byte fields that make up bytes between them, in one group or in two saying the same. */
  EXPECT(loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] block { a: 3; b: 5; }; };\n"));
  EXPECT(loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] a: 3; [ @if: valof(f) == 1; ] b: 5; };\n"));
  /* A nested part is its own part: the outer's three bits do not make up the inner's five. */
  EXPECT(refused(
    "pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] block { a: 3; [ @if: valof(a) == 1; ] b: 5; }; };\n", 13, NULL
  ));
  EXPECT(refused("pkt T { id: 8 := 1; f: 8; n: 8; [ @if: valof(f) == 1; ] d[4]: valof(n) * 3; };\n", 13, NULL));
  EXPECT(loads("pkt T { id: 8 := 1; f: 8; n: 8; [ @if: valof(f) == 1; ] d[4]: valof(n) * 8; };\n"));
  /* What every frame carries is still held to whole bytes on its own. */
  EXPECT(refused("pkt T { id: 8 := 1; f: 4; [ @if: valof(f) == 1; ] e: 8; };\n", 13, NULL));
  return failure;
}

/* Check 11: a fixed value identifies a packet only where every frame has it, at a known position. */
static int us24_tc04_a_discriminator_is_carried_at_a_known_position(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(
    refused("pkt T { f: 8; [ @if: valof(f) == 1; ] id: 8 := 1; };\n", 11, "'id' is present only when valof(f) == 1")
  );
  EXPECT(refused(
    "pkt T { f: 8; [ @if: valof(f) == 1; ] e: 16; id: 8 := 1; };\n", 11, "'id' comes after 'e', which is conditional"
  ));
  EXPECT(refused("pkt T { n: 8; d[4]: valof(n) * 8; id: 8 := 1; };\n", 11, "which is sized by another field"));
  EXPECT(refused("pkt T { f: 8; [ @if: valof(f) == 1; ] block { e: 8; id: 8 := 1; }; };\n", 11, NULL));
  /* One before the first conditional field is enough; one inside a part is written when it is there. */
  EXPECT(loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] block { sync: 8 := 0xAA; e: 8; }; };\n"));
  Fixture fixture;
  if (!fixture_open(
        &fixture, "pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] block { sync: 8 := 0xAA; e: 8; }; };\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  EXPECT(encodes(&fixture, "T", "01 00"));
  EXPECT(set(&fixture, "T.f", 1) == FRAMEDSL_OK && set(&fixture, "T.e", 2) == FRAMEDSL_OK);
  EXPECT(encodes(&fixture, "T", "01 01 AA 02"));
  fixture_close(&fixture);
  return failure;
}

/* Check 12: a fixed value inside a conditional part does not tell two packets apart. */
static int us24_tc05_a_conditional_value_does_not_tell_packets_apart(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused(
    "pkt A { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] k: 8 := 1; };\n"
    "pkt B { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] k: 8 := 2; };\n",
    12,
    NULL
  ));
  /* Told apart before the part, they load, and decode by what precedes it. */
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt A { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] k: 8; };\n"
        "pkt B { id: 8 := 2; f: 8; [ @if: valof(f) == 1; ] k: 8; };\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  EXPECT(decodes_as(&fixture, "01 01 05", "A") && decodes_as(&fixture, "02 00", "B"));
  EXPECT(decodes_as(&fixture, "02 01 06", "B") && get(&fixture, "B.k") == 6);
  fixture_close(&fixture);
  return failure;
}

/* Checks 8 and 39: what a field is compared with is a value it can hold, written as its type says. */
static int us24_tc06_a_condition_compares_with_a_value_of_the_fields_type(const TestArguments *args) {
  (void)args;
  int failure = 0;
  const char *const kinds = "enum K: 4 { IDLE; RUN; };\n";
  char source[512];

  snprintf(source, sizeof(source), "%spkt T { id: 8 := 1; k: K; p: 4; [ @if: valof(k) == 1; ] x: 8; };\n", kinds);
  EXPECT(refused(source, 39, "write 'RUN' for 1"));
  snprintf(source, sizeof(source), "%spkt T { id: 8 := 1; k: K; p: 4; [ @if: valof(k) == 9; ] x: 8; };\n", kinds);
  EXPECT(refused(source, 39, "not a number"));
  snprintf(source, sizeof(source), "%spkt T { id: 8 := 1; k: K; p: 4; [ @if: valof(k) == RNU; ] x: 8; };\n", kinds);
  EXPECT(refused(source, 39, "no name 'RNU'"));
  FramedslResult *result = NULL;
  const FramedslDiagnostic *typo = refusal(source, 39, &result);
  EXPECT(typo != NULL && typo->fix_text != NULL && strcmp(typo->fix_text, "RUN") == 0);
  framedsl_result_free(result);
  EXPECT(refused("pkt T { id: 8 := 1; k: 8; [ @if: valof(k) == RUN; ] x: 8; };\n", 39, "not typed by an enum"));
  EXPECT(refused("pkt T { id: 8 := 1; k: 4; p: 4; [ @if: valof(k) == 16; ] x: 8; };\n", 8, "can never be 0x10"));
  EXPECT(loads("pkt T { id: 8 := 1; k: 4; p: 4; [ @if: valof(k) != 15; ] x: 8; };\n"));
  EXPECT(loads("pkt T { id: 8 := 1; k: 64; [ @if: valof(k) == 0xFFFFFFFFFFFFFFFF; ] x: 8; };\n"));
  /* A name from another enum is not this one's. */
  snprintf(
    source,
    sizeof(source),
    "%senum J: 4 { OTHER; };\npkt T { id: 8 := 1; k: K; p: 4; [ @if: valof(k) == OTHER; ] x: 8; };\n",
    kinds
  );
  EXPECT(refused(source, 39, NULL));
  return failure;
}

/* Checks 7 and 16: what a condition reads is a scalar, already read. */
static int us24_tc07_a_condition_reads_a_scalar_already_read(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt T { id: 8 := 1; [ @if: valof(f) == 1; ] e: 8; f: 8; };\n", 7, "declared later"));
  EXPECT(refused("pkt T { id: 8 := 1; [ @if: valof(e) == 1; ] e: 8; };\n", 7, "the one it decides"));
  EXPECT(refused("pkt T { id: 8 := 1; flag: 8; [ @if: valof(flga) == 1; ] e: 8; };\n", 7, "names no field"));
  FramedslResult *result = NULL;
  const FramedslDiagnostic *typo =
    refusal("pkt T { id: 8 := 1; flag: 8; [ @if: valof(flga) == 1; ] e: 8; };\n", 7, &result);
  EXPECT(typo != NULL && typo->fix_text != NULL && strcmp(typo->fix_text, "flag") == 0);
  framedsl_result_free(result);
  /* A did-you-mean through an instance offers the part written, not the instance's prefix. */
  typo =
    refusal("block H { flag: 8; };\npkt T { id: 8 := 1; h: H; [ @if: valof(h.flga) == 1; ] e: 8; };\n", 7, &result);
  EXPECT(typo != NULL && typo->fix_text != NULL && strcmp(typo->fix_text, "h.flag") == 0);
  framedsl_result_free(result);
  EXPECT(refused("pkt T { id: 8 := 1; b: 72; [ @if: valof(b) == 1; ] e: 8; };\n", 16, NULL));
  EXPECT(refused("block I { v: 8; };\npkt T { id: 8 := 1; is: I[2]; [ @if: valof(is) == 1; ] e: 8; };\n", 16, NULL));
  /* Each mistake in a group's condition is reported once, not once per field it covers. */
  EXPECT(refused_once("pkt T { id: 8 := 1; [ @if: valof(nope) == 1; ] block { a: 8; b: 8; c: 8; }; };\n", "nope"));
  return failure;
}

/* Check 32: a part of an element cannot be conditional; a whole repetition can. */
static int us24_tc08_a_repetition_is_conditional_only_as_a_whole(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(
    refused("block S { k: 8; [ @if: valof(k) == 1; ] e: 8; };\npkt T { id: 8 := 1; s: S[2]; };\n", 32, "conditional")
  );
  EXPECT(refused(
    "block S { k: 8; [ @if: valof(k) == 1; ] e: 8; };\npkt T { id: 8 := 1; n: 8; s[8]: S[valof(n)]; };\n", 32, NULL
  ));
  EXPECT(loads("block S { k: 8; e: 8; };\npkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] s: S[2]; };\n"));
  /* A block with a conditional field is still usable once, named or pasted. */
  EXPECT(loads("block S { k: 8; [ @if: valof(k) == 1; ] e: 8; };\npkt T { id: 8 := 1; s: S; };\n"));
  EXPECT(loads("block S { k: 8; [ @if: valof(k) == 1; ] e: 8; };\npkt T { id: 8 := 1; S; };\n"));
  return failure;
}

/* Where `@if` may be written, and how. */
static int us24_tc09_if_goes_in_front_of_what_may_be_absent(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused_once("@if: valof(x) == 1;\npkt T { id: 8 := 1; };\n", "a whole file"));
  EXPECT(refused_once("[ @if: valof(x) == 1; ] pkt T { id: 8 := 1; };\n", "a packet"));
  EXPECT(refused_once("[ @if: valof(x) == 1; ] block B { x: 8; };\npkt T { id: 8 := 1; b: B; };\n", "named block"));
  EXPECT(
    refused_once("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; @if: valof(f) == 2; ] e: 8; };\n", "written twice")
  );
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: f == 1; ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) = 1; ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) ! 1; ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == ; ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1 ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) + 1 == 1; ] e: 8; };\n"));
  EXPECT(!loads("enum K: 8 { A; };\npkt T { id: 8 := 1; f: K; [ @if: valof(f) == K.A; ] e: 8; };\n"));
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1.5; ] e: 8; };\n"));
  /* Nothing to apply to. */
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) == 1; ] };\n"));
  /* In front of an enum's declaration, as any group is. */
  EXPECT(!loads("[ @if: valof(f) == 1; ] enum K: 8 { A; };\npkt T { id: 8 := 1; };\n"));
  /* The unknown-attribute message lists it. */
  EXPECT(refused_once("pkt T { id: 8 := 1; [ @iff: valof(id) == 1; ] e: 8; };\n", "'decode' and 'if'"));
  return failure;
}

/* `if` is a word only after `@`, as every attribute's name is. */
static int us24_tc10_a_field_may_still_be_called_if(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(loads("pkt T { if: 8 := 1; [ @if: valof(if) == 1; ] e: 8; };\n"));
  EXPECT(loads("block if { if: 8; };\npkt T { id: 8 := 1; i: if; [ @if: valof(i.if) == 2; ] e: 8; };\n"));
  /* `==` written apart is two `=`, which no declaration takes. */
  EXPECT(!loads("pkt T { id: 8 := 1; f: 8; [ @if: valof(f) = = 1; ] e: 8; };\n"));
  return failure;
}

static int us24_tc11_a_conditional_field_describes_its_condition(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture, SHAPES)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  size_t packet = 0;
  EXPECT(framedsl_find_packet(fixture.definition, "S", &packet));
  const char *const expected[][2] = {
    { "id",      NULL            },
    { "m",       NULL            },
    { "named.a", "valof(m) == 1" },
    { "named.b", "valof(m) == 1" },
    { "a",       "valof(m) == 2" },
    { "b",       "valof(m) == 2" },
    { "c",       "valof(m) == 3" },
    { "d",       "valof(m) == 3" },
    { "n",       NULL            },
    { "items",   "valof(m) == 4" },
    { "end",     NULL            },
  };
  EXPECT(framedsl_field_count(fixture.definition, packet) == sizeof(expected) / sizeof(*expected));
  for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
    FramedslField field;
    EXPECT(framedsl_field(fixture.definition, packet, i, &field) && strcmp(field.name, expected[i][0]) == 0);
    if (expected[i][1] == NULL) {
      EXPECT(field.condition == NULL);
    } else {
      EXPECT(field.condition != NULL && strcmp(field.condition, expected[i][1]) == 0);
    }
  }
  /* A member describes no condition of its own: it is present when its repetition is. */
  FramedslField member;
  EXPECT(framedsl_member(fixture.definition, packet, 9, 0, &member) && member.condition == NULL);
  fixture_close(&fixture);

  /* Nested deep, a description stops growing and says so: it would otherwise grow with the square of
   * the depth, every level holding all of the ones above it. */
  static char deep[8192];
  size_t used = (size_t)snprintf(deep, sizeof(deep), "pkt D { id: 8 := 1; f: 8;\n");
  for (int i = 0; i < 60; ++i) {
    used += (size_t)snprintf(deep + used, sizeof(deep) - used, "[ @if: valof(f) != %d; ] block {\n", i);
  }
  used += (size_t)snprintf(deep + used, sizeof(deep) - used, "x: 8;\n");
  for (int i = 0; i < 60; ++i) {
    used += (size_t)snprintf(deep + used, sizeof(deep) - used, "};\n");
  }
  snprintf(deep + used, sizeof(deep) - used, "};\n");
  if (!fixture_open(&fixture, deep)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  FramedslField x;
  EXPECT(framedsl_field(fixture.definition, 0, 2, &x) && x.condition != NULL);
  EXPECT(x.condition != NULL && strncmp(x.condition, "valof(f) != 0 && valof(f) != 1 && ", 34) == 0);
  EXPECT(x.condition != NULL && strlen(x.condition) < 512);
  EXPECT(x.condition != NULL && strcmp(x.condition + strlen(x.condition) - 7, " && ...") == 0);
  EXPECT(set(&fixture, "D.f", 59) == FRAMEDSL_OK && encodes(&fixture, "D", "01 3B"));
  EXPECT(set(&fixture, "D.f", 60) == FRAMEDSL_OK && encodes(&fixture, "D", "01 3C 00"));
  fixture_close(&fixture);
  return failure;
}

static int us24_tc12_the_cli_shows_conditions_and_absent_fields(const TestArguments *args) {
  int failure = 0;
  char command[2048];
  char output[8192];
  EXPECT(write_file(
    args,
    "cond.fdsl",
    "enum K: 4 { IDLE; RUN; };\n"
    "pkt T {\n"
    "  id: 8 := 0x10;\n"
    "  k:  K;\n"
    "  f:  4;\n"
    "  [@if:valof(k)!=IDLE;] e: 8;\n"
    "  [ @if: valof(f) == 1; @byte_order: little_endian; ] w: 16;\n"
    "};\n"
  ));

  snprintf(command, sizeof(command), "%s %s/cond.fdsl 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  EXPECT(run_capture(command, output, sizeof(output)) == 0);
  EXPECT(strstr(output, "8 bits, if valof(k) != IDLE") != NULL);
  EXPECT(strstr(output, "at ?, 16 bits, if valof(f) == 1") != NULL);

  snprintf(command, sizeof(command), "%s %s/cond.fdsl -d '10 01 34 12' 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  EXPECT(run_capture(command, output, sizeof(output)) == 0);
  EXPECT(strstr(output, "    e              <absent>\n") != NULL);
  EXPECT(strstr(output, "    w              4660 (0x1234)\n") != NULL);

  /* The formatter lays a condition out as an expression, and leaves its own output alone. */
  snprintf(
    command,
    sizeof(command),
    "%s %s/cond.fdsl --format 2>&1 && cat %s/cond.fdsl",
    FRAMEDSL_BIN,
    args->tmp_dir,
    args->tmp_dir
  );
  EXPECT(run_capture(command, output, sizeof(output)) == 0);
  EXPECT(strstr(output, "  [ @if: valof(k) != IDLE; ]\n  e:  8;\n") != NULL);
  EXPECT(strstr(output, "    @if:         valof(f) == 1;\n    @byte_order: little_endian;\n") != NULL);
  snprintf(command, sizeof(command), "%s %s/cond.fdsl --format --check 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  EXPECT(run_capture(command, output, sizeof(output)) == 0);

  EXPECT(
    write_file(args, "bad.fdsl", "pkt T { id: 8 := 1; h: 8; [ @if: valof(h) == 1; ] n: 8; d[4]: valof(n) * 8; };\n")
  );
  snprintf(command, sizeof(command), "%s %s/bad.fdsl --check 2>&1", FRAMEDSL_BIN, args->tmp_dir);
  EXPECT(run_capture(command, output, sizeof(output)) == 1);
  EXPECT(strstr(output, "bad.fdsl:1:63: error:") != NULL && strstr(output, "[check 42]") != NULL);
  return failure;
}

/* ---- Round trips ---------------------------------------------------------------------------------- */

static const char *const RANDOM =
  "enum K: 4 { NONE; ONE; TWO; };\n"
  "block Opt { kind: K; flags: 4; [ @if: valof(kind) == ONE; ] x: 8;\n"
  "  [ @if: valof(flags) != 0; ] block { n: 8; [ @if: valof(n) == 3; @byte_order: little_endian; ] y: 16; }; };\n"
  "block H { type: 8; f: 8; [ @if: valof(type) == 0x41; ] ext: 8; };\n"
  "block Pt { a: 8; [ @bit_order: lsb_first; ] b: 8; };\n"
  "pkt C1 { id: 8 := 0x40; flag: 1; pad: 7; [ @if: valof(flag) == 1; ] o: Opt; [ @if: valof(flag) != 1; ] Pt;\n"
  "  len: 8; [ @if: valof(len) != 0; ] d[32]: valof(len) * 8; };\n"
  "pkt C2 { h: H { type := 0x41; }; c: 8; [ @if: valof(h.f) == 1; ] pts[16]: Pt[valof(c)];\n"
  "  [ @if: valof(h.f) == 2; @byte_order: little_endian; ] w: 32; tail: 8; };\n";

static uint64_t next_random(uint64_t *state) {
  *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
  return *state >> 17;
}

/* A seeded walk: random values, encoded, decoded into a second frame, which must hold the same fields
 * present with the same values and encode the same bytes. Values are drawn small enough that every
 * condition both holds and fails across the run. */
static int us24_tc13_random_frames_round_trip(const TestArguments *args) {
  (void)args;
  /* Two frames of one definition: a field id belongs to the definition it was found in. */
  Fixture sender;
  bool opened = fixture_open(&sender, RANDOM);
  Fixture receiver = sender;
  receiver.frame = opened ? framedsl_frame_new(sender.definition) : NULL;
  if (receiver.frame == NULL) {
    framedsl_frame_free(receiver.frame);
    fixture_close(&sender);
    FORCE_FAILURE();
  }
  int failure = 0;
  uint64_t state = 12;
  size_t present_seen = 0;
  size_t absent_seen = 0;

  for (int round = 0; round < 3000 && failure == 0; ++round) {
    const char *packet = round % 2 == 0 ? "C1" : "C2";
    size_t index = 0;
    EXPECT(framedsl_find_packet(sender.definition, packet, &index));
    framedsl_frame_reset(sender.frame);

    for (size_t f = 0; f < framedsl_field_count(sender.definition, index); ++f) {
      FramedslField field;
      FramedslFieldId id;
      EXPECT(
        framedsl_field(sender.definition, index, f, &field) && framedsl_field_id(sender.definition, index, f, &id)
      );
      if (field.has_fixed) {
        continue;
      }
      if (field.is_scalar) {
        /* Mostly 0 to 3, so conditions comparing with 0, 1, 2 and 3 go both ways; sometimes anything. */
        uint64_t mask = field.width_bits >= 64 ? UINT64_MAX : ((uint64_t)1 << field.width_bits) - 1;
        uint64_t value = next_random(&state) % 4 == 0 ? next_random(&state) & mask : (next_random(&state) % 4) & mask;
        if (strcmp(field.name, "len") == 0) {
          value = next_random(&state) % 5;
        } else if (strcmp(field.name, "c") == 0) {
          value = next_random(&state) % 9;
        }
        EXPECT(framedsl_set_u64(sender.frame, id, value) == FRAMEDSL_OK);
      } else if (field.element_bits > 0) {
        for (size_t e = 0; e < 8; ++e) {
          char name[64];
          FramedslFieldId element;
          snprintf(name, sizeof(name), "%s.%s[%zu].a", packet, field.name, e);
          EXPECT(framedsl_find(sender.definition, name, &element));
          EXPECT(framedsl_set_u64(sender.frame, element, next_random(&state) & 0xFF) == FRAMEDSL_OK);
          snprintf(name, sizeof(name), "%s.%s[%zu].b", packet, field.name, e);
          EXPECT(framedsl_find(sender.definition, name, &element));
          EXPECT(framedsl_set_u64(sender.frame, element, next_random(&state) & 0xFF) == FRAMEDSL_OK);
        }
      } else {
        uint8_t bytes[8];
        uint64_t len = 0;
        EXPECT(framedsl_get_u64(sender.frame, id_of(&sender, "C1.len"), &len) == FRAMEDSL_OK);
        for (size_t i = 0; i < sizeof(bytes); ++i) {
          bytes[i] = (uint8_t)next_random(&state);
        }
        EXPECT(framedsl_set_bytes(sender.frame, id, bytes, (size_t)len) == FRAMEDSL_OK);
      }
    }

    size_t length = 0;
    FramedslStatus status = FRAMEDSL_OK;
    const uint8_t *encoded = framedsl_encode(sender.frame, packet, &length, &status);
    EXPECT(encoded != NULL);
    if (encoded == NULL) {
      break;
    }
    uint8_t copy[128];
    memcpy(copy, encoded, length);
    size_t consumed = 0;
    const char *name = framedsl_decode(receiver.frame, copy, length, &consumed, &status);
    EXPECT(name != NULL && strcmp(name, packet) == 0 && consumed == length);

    for (size_t f = 0; f < framedsl_field_count(sender.definition, index) && failure == 0; ++f) {
      FramedslField field;
      FramedslFieldId id;
      EXPECT(
        framedsl_field(sender.definition, index, f, &field) && framedsl_field_id(sender.definition, index, f, &id)
      );
      bool sent = false;
      bool received = false;
      EXPECT(framedsl_present(sender.frame, id, &sent) == FRAMEDSL_OK);
      EXPECT(framedsl_present(receiver.frame, id, &received) == FRAMEDSL_OK && sent == received);
      present_seen += field.condition != NULL && sent;
      absent_seen += !sent;
      if (!sent || field.has_fixed) {
        continue;
      }
      if (field.is_scalar) {
        uint64_t a = 0;
        uint64_t b = 0;
        EXPECT(
          framedsl_get_u64(sender.frame, id, &a) == FRAMEDSL_OK
          && framedsl_get_u64(receiver.frame, id, &b) == FRAMEDSL_OK
        );
        EXPECT(a == b);
      } else {
        size_t a_length = 0;
        size_t b_length = 0;
        const uint8_t *a = framedsl_get_bytes(sender.frame, id, &a_length, NULL);
        const uint8_t *b = framedsl_get_bytes(receiver.frame, id, &b_length, NULL);
        EXPECT(a != NULL && b != NULL);
        /* A repetition's slot holds every element set; only the counted ones were sent. */
        size_t count = 0;
        if (field.element_bits > 0) {
          EXPECT(framedsl_count(receiver.frame, id, &count) == FRAMEDSL_OK);
          a_length = b_length = count * (size_t)(field.element_bits / 8);
        }
        EXPECT(a_length == b_length && (a_length == 0 || memcmp(a, b, a_length) == 0));
      }
    }
    const uint8_t *again = framedsl_encode(receiver.frame, packet, &consumed, &status);
    EXPECT(again != NULL && consumed == length && memcmp(again, copy, length) == 0);
  }
  /* The walk went both ways often enough to mean something. */
  EXPECT(present_seen > 1000 && absent_seen > 1000);
  framedsl_frame_free(receiver.frame);
  fixture_close(&sender);
  return failure;
}

void register_user_story24(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us24_tc01_a_condition_shares_its_group, "a condition shares its group", false);
  ADD_TEST_CASE(
    us24_tc02_a_valof_reads_only_a_field_that_is_there, "a valof() reads only a field that is there", false
  );
  ADD_TEST_CASE(us24_tc03_a_conditional_part_is_whole_bytes, "a conditional part is whole bytes", false);
  ADD_TEST_CASE(
    us24_tc04_a_discriminator_is_carried_at_a_known_position, "a discriminator is at a known position", false
  );
  ADD_TEST_CASE(
    us24_tc05_a_conditional_value_does_not_tell_packets_apart, "a conditional value tells nothing apart", false
  );
  ADD_TEST_CASE(
    us24_tc06_a_condition_compares_with_a_value_of_the_fields_type, "compared with a value of its type", false
  );
  ADD_TEST_CASE(us24_tc07_a_condition_reads_a_scalar_already_read, "a condition reads a scalar already read", false);
  ADD_TEST_CASE(
    us24_tc08_a_repetition_is_conditional_only_as_a_whole, "a repetition is conditional only as a whole", false
  );
  ADD_TEST_CASE(us24_tc09_if_goes_in_front_of_what_may_be_absent, "@if goes in front of what may be absent", false);
  ADD_TEST_CASE(us24_tc10_a_field_may_still_be_called_if, "a field may still be called if", false);
  ADD_TEST_CASE(us24_tc11_a_conditional_field_describes_its_condition, "a field describes its condition", false);
  ADD_TEST_CASE(
    us24_tc12_the_cli_shows_conditions_and_absent_fields, "the CLI shows conditions and absent fields", false
  );
  ADD_TEST_CASE(us24_tc13_random_frames_round_trip, "random frames round trip", false);
}
