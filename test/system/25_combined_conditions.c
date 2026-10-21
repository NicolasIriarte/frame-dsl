/* Story 25: conditions that combine comparisons (#126) -- `&&`, `||` and parentheses, the orderings
 * `<`, `<=`, `>`, `>=`, and a mask, `valof(flags) & 0x4 != 0`.
 *
 * What each operator means, decided over every value a small field can hold; what a file may not say
 * with them; and a frame whose parts come and go with combined conditions, encoded and decoded for
 * every combination of the fields they read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

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
  fixture->result = framedsl_load_string(source, "combined.fdsl");
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

/* Whether `name` is present in the fixture's frame; false also when it cannot be asked. */
static bool present(const Fixture *fixture, const char *name) {
  bool there = false;
  return framedsl_present(fixture->frame, id_of(fixture, name), &there) == FRAMEDSL_OK && there;
}

/* The diagnostic `source` is refused with for check `wanted`, with `text` in it unless NULL. */
static bool refused(const char *source, int wanted, const char *text) {
  FramedslResult *result = framedsl_load_string(source, "combined.fdsl");
  bool found = false;
  for (size_t i = 0; !framedsl_ok(result) && i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diagnostic = framedsl_diagnostic(result, i);
    found = found
            || (diagnostic->check == wanted && diagnostic->severity == FRAMEDSL_SEVERITY_ERROR
                && (text == NULL || strstr(diagnostic->message, text) != NULL));
  }
  framedsl_result_free(result);
  return found;
}

static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "combined.fdsl");
  bool ok = framedsl_ok(result);
  framedsl_result_free(result);
  return ok;
}

/* The bytes `packet` encodes to, into `out`; the length, or 0 when it did not encode. */
static size_t encoded(Fixture *fixture, const char *packet, uint8_t *out, size_t capacity) {
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, NULL);
  if (bytes == NULL || length > capacity) {
    return 0;
  }
  memcpy(out, bytes, length);
  return length;
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

/* `&&` binds tighter than `||`, and parentheses group: decided over all eight combinations. */
static int us25_tc01_and_binds_tighter_than_or(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt P {\n"
        "  id: 8 = 0x5A;\n"
        "  a: 1; b: 1; c: 1; pad: 5;\n"
        "  [ @if: valof(a) == 1 || valof(b) == 1 && valof(c) == 1; ] x: 8;\n"
        "  [ @if: (valof(a) == 1 || valof(b) == 1) && valof(c) == 1; ] y: 8;\n"
        "  [ @if: valof(a) == 1 || (valof(b) == 1 && valof(c) == 1); ] z: 8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  for (unsigned bits = 0; bits < 8; ++bits) {
    bool a = (bits & 1) != 0, b = (bits & 2) != 0, c = (bits & 4) != 0;
    EXPECT(set(&fixture, "P.a", a) == FRAMEDSL_OK && set(&fixture, "P.b", b) == FRAMEDSL_OK);
    EXPECT(set(&fixture, "P.c", c) == FRAMEDSL_OK);
    bool x = a || (b && c), y = (a || b) && c;
    EXPECT(present(&fixture, "P.x") == x);
    EXPECT(present(&fixture, "P.y") == y);
    EXPECT(present(&fixture, "P.z") == x);
    uint8_t bytes[8];
    EXPECT(encoded(&fixture, "P", bytes, sizeof(bytes)) == 2u + x + y + x);
  }
  fixture_close(&fixture);
  return failure;
}

/* `<`, `<=`, `>` and `>=` compare a field's value with a number, over every value a 4-bit field holds. */
static int us25_tc02_orderings_compare_with_a_number(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt P {\n"
        "  id: 8 = 0x5A;\n"
        "  v: 4; pad: 4;\n"
        "  [ @if: valof(v) < 5; ]  lt: 8;\n"
        "  [ @if: valof(v) <= 5; ] le: 8;\n"
        "  [ @if: valof(v) > 5; ]  gt: 8;\n"
        "  [ @if: valof(v) >= 5; ] ge: 8;\n"
        "  [ @if: valof(v) >= 2 && valof(v) < 12; ] range: 8;\n"
        "  [ @if: valof(v) < 2 || valof(v) >= 0xF; ] outside: 8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  for (uint64_t v = 0; v < 16; ++v) {
    EXPECT(set(&fixture, "P.v", v) == FRAMEDSL_OK);
    EXPECT(present(&fixture, "P.lt") == (v < 5));
    EXPECT(present(&fixture, "P.le") == (v <= 5));
    EXPECT(present(&fixture, "P.gt") == (v > 5));
    EXPECT(present(&fixture, "P.ge") == (v >= 5));
    EXPECT(present(&fixture, "P.range") == (v >= 2 && v < 12));
    EXPECT(present(&fixture, "P.outside") == (v < 2 || v >= 15));
  }
  fixture_close(&fixture);
  return failure;
}

/* A mask tests bits, and binds tighter than the comparison: `valof(flags) & 0x4 != 0` is bit 2, not
 * C's `flags & (4 != 0)`, which would be bit 0. Over every value of an 8-bit field. */
static int us25_tc03_a_mask_tests_bits_of_a_field(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(
        &fixture,
        "pkt P {\n"
        "  id: 8 = 0x5A;\n"
        "  flags: 8;\n"
        "  [ @if: valof(flags) & 0x4 != 0; ]    bit2: 8;\n"
        "  [ @if: valof(flags) & 0xC == 0x8; ]  bits32: 8;\n"
        "  [ @if: valof(flags) & 0xF0 >= 0x30; ] high: 8;\n"
        "  [ @if: valof(flags) & 0x1 == 1 && valof(flags) & 0x80 == 0; ] both: 8;\n"
        "};\n"
      )) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  for (uint64_t flags = 0; flags < 256; ++flags) {
    EXPECT(set(&fixture, "P.flags", flags) == FRAMEDSL_OK);
    EXPECT(present(&fixture, "P.bit2") == ((flags & 0x4) != 0));
    EXPECT(present(&fixture, "P.bits32") == ((flags & 0xC) == 0x8));
    EXPECT(present(&fixture, "P.high") == ((flags & 0xF0) >= 0x30));
    EXPECT(present(&fixture, "P.both") == ((flags & 0x1) == 1 && (flags & 0x80) == 0));
  }
  fixture_close(&fixture);
  return failure;
}

static const char *const KIND = "enum Kind: 2 { IDLE; HK; EVENT; DUMP; };\n";

/* An enum's values are names: they are equal or not, and several are accepted by writing each. An
 * ordering or a mask on a field typed by one is check 43, and says how to write what was meant. */
static int us25_tc04_an_enum_field_is_compared_only_for_equality(const TestArguments *args) {
  (void)args;
  int failure = 0;
  char source[1024];
  static const char *const accepted[] = {
    "valof(kind) == HK || valof(kind) == EVENT",
    "valof(kind) != IDLE && valof(kind) != DUMP",
    "(valof(kind) == HK || valof(kind) == DUMP) && valof(n) > 3",
  };
  for (size_t i = 0; i < sizeof(accepted) / sizeof(*accepted); ++i) {
    snprintf(
      source, sizeof(source), "%spkt P { id: 8 = 1; kind: Kind; n: 6; [ @if: %s; ] x: 8; };\n", KIND, accepted[i]
    );
    EXPECT(loads(source));
  }
  static const char *const refused_here[] = {
    "valof(kind) >= HK",
    "valof(kind) < EVENT",
    "valof(kind) > 1",
    "valof(kind) & 0x1 == 1",
    "valof(n) == 1 || valof(kind) <= HK",
  };
  for (size_t i = 0; i < sizeof(refused_here) / sizeof(*refused_here); ++i) {
    snprintf(
      source, sizeof(source), "%spkt P { id: 8 = 1; kind: Kind; n: 6; [ @if: %s; ] x: 8; };\n", KIND, refused_here[i]
    );
    EXPECT(refused(source, 43, "'valof(kind) == IDLE || valof(kind) == HK'"));
  }
  /* Through an instance, the suggestion names the field as written there. */
  snprintf(
    source,
    sizeof(source),
    "%sblock H { kind: Kind; n: 6; };\npkt P { id: 8 = 1; hdr: H; [ @if: valof(hdr.kind) > HK; ] x: 8; };\n",
    KIND
  );
  EXPECT(refused(source, 43, "'valof(hdr.kind) == IDLE"));
  return failure;
}

/* Check 8 holds for every comparison, and for a mask: what it compares with fits the field, and an
 * equality the mask alone decides is refused. An ordering against a value the mask clears still means
 * something, and loads. */
static int us25_tc05_numbers_fit_the_field_they_are_compared_with(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(
    refused("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) & 0x10 == 0; ] x: 8; };\n", 8, "masking it with 0x10")
  );
  EXPECT(refused("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) & 0x4 == 0x8; ] x: 8; };\n", 8, "never true"));
  EXPECT(refused("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) & 0x4 != 0x8; ] x: 8; };\n", 8, "always true"));
  EXPECT(refused("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) > 16; ] x: 8; };\n", 8, "can never be 0x10"));
  EXPECT(refused("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) == 1 || valof(f) >= 99; ] x: 8; };\n", 8, NULL));
  EXPECT(loads("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) & 0xC < 0x6; ] x: 8; };\n"));
  /* A mask is a number, not a name, and the operator after it is still a comparison. */
  EXPECT(!loads("enum K: 4 { A; B; };\npkt P { id: 8 = 1; f: 4; k: K; [ @if: valof(f) & A == 0; ] x: 8; };\n"));
  EXPECT(!loads("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) | 1 == 1; ] x: 8; };\n"));
  EXPECT(!loads("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: valof(f) == 1 &&; ] x: 8; };\n"));
  EXPECT(!loads("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: (valof(f) == 1; ] x: 8; };\n"));
  EXPECT(!loads("pkt P { id: 8 = 1; f: 4; pad: 4; [ @if: !(valof(f) == 1); ] x: 8; };\n"));
  return failure;
}

/* Check 42 compares terms -- what `&&` joins, in an `@if` and the groups around it -- as written. A term
 * the read field needs must be among the reader's; an implication is not enough. Check 13 counts the
 * fields under the same terms, in any order, as one part. */
static int us25_tc06_conditions_are_compared_by_their_terms(const TestArguments *args) {
  (void)args;
  int failure = 0;
  const char *const head = "pkt P { id: 8 = 1; f: 4; h: 4;\n";
  char source[1024];

#define WITH(body) (snprintf(source, sizeof(source), "%s%s};\n", head, body), source)
  /* The same term, written the same way: fine. */
  EXPECT(loads(WITH(
    "[ @if: valof(f) == 1 || valof(f) == 2; ] n: 8;\n"
    "[ @if: valof(f) == 1 || valof(f) == 2; ] block { d[4]: valof(n) * 8; };\n"
  )));
  /* Implied, but another term: refused. */
  EXPECT(refused(
    WITH("[ @if: valof(f) == 1 || valof(f) == 2; ] n: 8;\n[ @if: valof(f) == 1; ] block { d[4]: valof(n) * 8; };\n"),
    42,
    "valof(f) == 1 || valof(f) == 2"
  ));
  /* `&&` in one `@if` is nesting by another name, either way round. */
  EXPECT(loads(WITH(
    "[ @if: valof(h) == 1; ] block { [ @if: valof(f) > 3; ] n: 8; };\n"
    "[ @if: valof(f) > 3 && valof(h) == 1; ] block { d[4]: valof(n) * 8; };\n"
  )));
  EXPECT(loads(WITH(
    "[ @if: valof(h) == 1 && valof(f) > 3; ] n: 8;\n"
    "[ @if: valof(h) == 1; ] block { [ @if: valof(f) > 3; ] d[4]: valof(n) * 8; };\n"
  )));
  /* A comparison reads what the terms to its left make present, not what the ones to its right do. */
  EXPECT(loads(WITH("[ @if: valof(h) == 1; ] e: 8;\n[ @if: valof(h) == 1 && valof(e) == 2; ] x: 8;\n")));
  EXPECT(refused(WITH("[ @if: valof(h) == 1; ] e: 8;\n[ @if: valof(e) == 2 && valof(h) == 1; ] x: 8;\n"), 42, NULL));
  /* Inside an `||`, nothing to the left counts: the right-hand side is asked when the left fails. */
  EXPECT(refused(WITH("[ @if: valof(h) == 1; ] e: 8;\n[ @if: valof(h) == 1 || valof(e) == 2; ] x: 8;\n"), 42, NULL));
  /* Check 13: the same terms in another order are one part, and two halves of a byte make one. */
  EXPECT(loads(WITH(
    "[ @if: valof(h) == 1 && valof(f) > 3; ] x: 4;\n"
    "[ @if: valof(f) > 3; ] block { [ @if: valof(h) == 1; ] y: 4; };\n"
  )));
  EXPECT(refused(WITH("[ @if: valof(h) == 1 && valof(f) > 3; ] x: 4;\n[ @if: valof(f) > 3; ] y: 4;\n"), 13, NULL));
  /* Two `@if` in one group is still refused, and the message says how to join them. */
  EXPECT(!loads(WITH("[ @if: valof(h) == 1; @if: valof(f) == 2; ] x: 8;\n")));
#undef WITH
  return failure;
}

static const char *const TELEMETRY =
  "enum Kind: 2 { IDLE; HK; EVENT; DUMP; };\n"
  "pkt T {\n"
  "  id:      8 = 0x7E;\n"
  "  version: 4;\n"
  "  flags:   4;\n"
  "  kind:    Kind;\n"
  "  spare:   6;\n"
  "  [ @if: valof(flags) & 0x1 != 0; ]\n"
  "  seq:     16;\n"
  "  [ @if: valof(version) >= 2 && valof(version) < 5; ]\n"
  "  ext:     8;\n"
  "  [ @if: valof(kind) == HK || valof(kind) == EVENT; ]\n"
  "  block {\n"
  "    n:    8;\n"
  "    [ @if: valof(flags) & 0x2 != 0 || valof(version) > 10; ]\n"
  "    code: 8;\n"
  "  };\n"
  "  [ @if: (valof(flags) & 0x8 == 0x8 || valof(version) == 0) && valof(kind) != IDLE; ]\n"
  "  crc:     16;\n"
  "  tail:    8;\n"
  "};\n";

/* A frame decodes and encodes each combined condition's fields present and absent correctly: every
 * combination of the three fields the conditions read, against bytes built here by hand, then decoded
 * into a second frame and encoded again. */
static int us25_tc07_combined_conditions_round_trip(const TestArguments *args) {
  (void)args;
  Fixture sender;
  Fixture receiver;
  if (!fixture_open(&sender, TELEMETRY) || !fixture_open(&receiver, TELEMETRY)) {
    fixture_close(&sender);
    fixture_close(&receiver);
    FORCE_FAILURE();
  }
  int failure = 0;
  EXPECT(set(&sender, "T.seq", 0x1234) == FRAMEDSL_OK && set(&sender, "T.ext", 0x56) == FRAMEDSL_OK);
  EXPECT(set(&sender, "T.n", 0x78) == FRAMEDSL_OK && set(&sender, "T.code", 0x9A) == FRAMEDSL_OK);
  EXPECT(set(&sender, "T.crc", 0xBCDE) == FRAMEDSL_OK && set(&sender, "T.tail", 0xF0) == FRAMEDSL_OK);

  static const char *const optional[] = { "T.seq", "T.ext", "T.n", "T.code", "T.crc" };
  for (uint64_t version = 0; version < 16; ++version) {
    for (uint64_t flags = 0; flags < 16; ++flags) {
      for (uint64_t kind = 0; kind < 4; ++kind) {
        EXPECT(set(&sender, "T.version", version) == FRAMEDSL_OK && set(&sender, "T.flags", flags) == FRAMEDSL_OK);
        EXPECT(set(&sender, "T.kind", kind) == FRAMEDSL_OK);

        const bool block = kind == 1 || kind == 2;
        const bool there[] = {
          (flags & 0x1) != 0,
          version >= 2 && version < 5,
          block,
          block && ((flags & 0x2) != 0 || version > 10),
          ((flags & 0x8) == 0x8 || version == 0) && kind != 0,
        };
        uint8_t want[16];
        size_t length = 0;
        want[length++] = 0x7E;
        want[length++] = (uint8_t)(version << 4 | flags);
        want[length++] = (uint8_t)(kind << 6);
        if (there[0]) {
          want[length++] = 0x12;
          want[length++] = 0x34;
        }
        if (there[1]) {
          want[length++] = 0x56;
        }
        if (there[2]) {
          want[length++] = 0x78;
        }
        if (there[3]) {
          want[length++] = 0x9A;
        }
        if (there[4]) {
          want[length++] = 0xBC;
          want[length++] = 0xDE;
        }
        want[length++] = 0xF0;

        uint8_t got[16];
        size_t got_length = encoded(&sender, "T", got, sizeof(got));
        EXPECT(got_length == length && memcmp(got, want, length) == 0);

        size_t consumed = 0;
        const char *name = framedsl_decode(receiver.frame, want, length, &consumed, NULL);
        EXPECT(name != NULL && strcmp(name, "T") == 0 && consumed == length);
        for (size_t i = 0; i < sizeof(optional) / sizeof(*optional); ++i) {
          EXPECT(present(&sender, optional[i]) == there[i]);
          EXPECT(present(&receiver, optional[i]) == there[i]);
          uint64_t sent = 0;
          uint64_t received = 0;
          FramedslStatus status = framedsl_get_u64(receiver.frame, id_of(&receiver, optional[i]), &received);
          EXPECT(status == (there[i] ? FRAMEDSL_OK : FRAMEDSL_ERROR_ABSENT));
          if (there[i]) {
            EXPECT(framedsl_get_u64(sender.frame, id_of(&sender, optional[i]), &sent) == FRAMEDSL_OK);
            EXPECT(sent == received);
          }
        }
        uint8_t again[16];
        EXPECT(encoded(&receiver, "T", again, sizeof(again)) == length && memcmp(again, want, length) == 0);
      }
    }
  }
  fixture_close(&sender);
  fixture_close(&receiver);
  return failure;
}

/* What the tools show: a field's description, the formatter's layout, and `-d` on a frame. */
static int us25_tc08_the_tools_show_combined_conditions(const TestArguments *args) {
  int failure = 0;
  Fixture fixture;
  if (!fixture_open(&fixture, TELEMETRY)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  const char *const expected[][2] = {
    { "seq",  "valof(flags) & 0x1 != 0"                                                                         },
    { "ext",  "valof(version) >= 2 && valof(version) < 5"                                                       },
    { "n",    "valof(kind) == HK || valof(kind) == EVENT"                                                       },
    { "code", "(valof(kind) == HK || valof(kind) == EVENT) && (valof(flags) & 0x2 != 0 || valof(version) > 10)" },
    { "crc",  "(valof(flags) & 0x8 == 8 || valof(version) == 0) && valof(kind) != IDLE"                         },
  };
  for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
    FramedslFieldId id;
    char dotted[32];
    snprintf(dotted, sizeof(dotted), "T.%s", expected[i][0]);
    FramedslField field;
    EXPECT(framedsl_find(fixture.definition, dotted, &id));
    EXPECT(framedsl_field(fixture.definition, id.packet, id.field, &field));
    EXPECT(field.condition != NULL && strcmp(field.condition, expected[i][1]) == 0);
    if (field.condition != NULL && strcmp(field.condition, expected[i][1]) != 0) {
      fprintf(stderr, "%s: '%s'\n", dotted, field.condition);
    }
  }
  fixture_close(&fixture);

  /* The formatter spaces every operator and keeps the parentheses written, redundant or not. */
  char *out = NULL;
  EXPECT(
    framedsl_format("pkt P { id: 8 = 1; f: 8;\n[ @if: (valof(f)&0x4!=0||valof(f)<=3)&&(valof(f)>1); ] x: 8; };\n", &out)
    == FRAMEDSL_OK
  );
  EXPECT(out != NULL && strstr(out, "[ @if: (valof(f) & 0x4 != 0 || valof(f) <= 3) && (valof(f) > 1); ]") != NULL);
  free(out);

  char path[1024];
  snprintf(path, sizeof(path), "%s/combined.fdsl", args->tmp_dir);
  FILE *file = fopen(path, "w");
  EXPECT(file != NULL);
  if (file != NULL) {
    fputs(TELEMETRY, file);
    fclose(file);
  }
  char command[2048];
  char output[8192];
  /* version 3, flags 0x1, kind HK: seq and ext and n there, code and crc not. */
  snprintf(command, sizeof(command), "%s %s -d '7E 31 40 12 34 56 78 F0' 2>&1", FRAMEDSL_BIN, path);
  EXPECT(run_capture(command, output, sizeof(output)) == 0);
  EXPECT(strstr(output, "code") != NULL && strstr(strstr(output, "code"), "<absent>") != NULL);
  EXPECT(strstr(output, "crc") != NULL && strstr(strstr(output, "crc"), "<absent>") != NULL);
  EXPECT(strstr(output, "ext") != NULL && strstr(output, "86 (0x56)") != NULL);
  snprintf(command, sizeof(command), "%s %s 2>&1", FRAMEDSL_BIN, path);
  EXPECT(run_capture(command, output, sizeof(output)) == 0);
  EXPECT(strstr(output, "if valof(version) >= 2 && valof(version) < 5") != NULL);
  return failure;
}

void register_user_story25(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us25_tc01_and_binds_tighter_than_or, "&& binds tighter than ||, and parentheses group", false);
  ADD_TEST_CASE(us25_tc02_orderings_compare_with_a_number, "<, <=, > and >= compare with a number", false);
  ADD_TEST_CASE(us25_tc03_a_mask_tests_bits_of_a_field, "a mask tests bits of a field", false);
  ADD_TEST_CASE(
    us25_tc04_an_enum_field_is_compared_only_for_equality, "an enum field is compared only for equality", false
  );
  ADD_TEST_CASE(
    us25_tc05_numbers_fit_the_field_they_are_compared_with, "numbers fit the field they are compared with", false
  );
  ADD_TEST_CASE(us25_tc06_conditions_are_compared_by_their_terms, "conditions are compared by their terms", false);
  ADD_TEST_CASE(us25_tc07_combined_conditions_round_trip, "combined conditions round trip", false);
  ADD_TEST_CASE(us25_tc08_the_tools_show_combined_conditions, "the tools show combined conditions", false);
}
