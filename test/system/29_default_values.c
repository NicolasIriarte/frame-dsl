/* Story 29: a default is what a field holds until something replaces it (#144).
 *
 * It is an ordinary value to everything but a load: it sits in the slot from the moment a frame starts,
 * a program or a decode replaces it, and it identifies nothing. The one other thing that changed with it
 * is how a value is spelt: `:=` fixes one, `?=` gives a default, and a bare `=` on a field is an error. */
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
  "enum Health: 8 { FAULT; OK; };\n"
  "block Channel { state: Health ?= OK; };\n"
  "block Pair { a: 8 ?= 1; b: 8 := 2; };\n"
  "block Item { mark: 4 := 0xA; level: 4 ?= 9; };\n"
  "pkt TM_HK {\n"
  "  id:       8 := 0x40;\n"
  "  spare:    8 ?= 0;\n"
  "  power:    Health ?= OK;\n"
  "  temp:     Health ?= OK;\n"
  "  channels: Channel[4];\n"
  "};\n"
  "pkt USES {\n"
  "  id: 8 := 0x41;\n"
  "  p:  Pair { a ?= 7; };\n"
  "  q:  Pair { a := 8; };\n"
  "  r:  Pair { b ?= 9; };\n"
  "};\n"
  "pkt OPTIONAL {\n"
  "  id:   8 := 0x42;\n"
  "  on:   8;\n"
  "  [ @if: valof(on) == 1; ]\n"
  "  opt:  8 ?= 0x55;\n"
  "  tail: 8;\n"
  "};\n"
  "pkt COUNTED {\n"
  "  id:       8 := 0x43;\n"
  "  count:    8 ?= 2;\n"
  "  items[4]: Item[valof(count)];\n"
  "};\n"
  "pkt SIZED {\n"
  "  id:      8 := 0x44;\n"
  "  size:    8 ?= 2;\n"
  "  data[4]: valof(size) * 8;\n"
  "};\n";

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(Fixture *fixture) {
  fixture->result = framedsl_load_string(DEFINITION, "defaults.fdsl");
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

/* Whether encoding `packet` gives exactly `expected`. */
static bool encodes(FramedslFrame *frame, const char *packet, const uint8_t *expected, size_t length) {
  size_t got = 0;
  FramedslStatus status = FRAMEDSL_OK;
  const uint8_t *bytes = framedsl_encode(frame, packet, &got, &status);
  return status == FRAMEDSL_OK && bytes != NULL && got == length && memcmp(bytes, expected, length) == 0;
}

static uint64_t read_u64(const Fixture *fixture, const char *dotted) {
  uint64_t value = UINT64_MAX;
  framedsl_get_u64(fixture->frame, id_of(fixture, dotted), &value);
  return value;
}

/* Whether `source` fails to load, on check `check` (0 for a syntax error), saying `text`. */
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

static int us29_tc01_nothing_set_encodes_the_defaults(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t expected[] = { 0x40, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
  EXPECT(encodes(fixture.frame, "TM_HK", expected, sizeof(expected)));
  EXPECT(read_u64(&fixture, "TM_HK.power") == 1);
  close_fixture(&fixture);
  return failure;
}

static int us29_tc02_setting_replaces_a_default_and_a_reset_brings_it_back(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(framedsl_set_name(fixture.frame, id_of(&fixture, "TM_HK.temp"), "FAULT") == FRAMEDSL_OK);
  EXPECT(framedsl_set_name(fixture.frame, id_of(&fixture, "TM_HK.channels[2].state"), "FAULT") == FRAMEDSL_OK);

  /* Encoding twice, because a fixed value is stamped on every encode and a default must never be. */
  const uint8_t changed[] = { 0x40, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01 };
  EXPECT(encodes(fixture.frame, "TM_HK", changed, sizeof(changed)));
  EXPECT(encodes(fixture.frame, "TM_HK", changed, sizeof(changed)));

  framedsl_frame_reset(fixture.frame);
  const uint8_t back[] = { 0x40, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 };
  EXPECT(encodes(fixture.frame, "TM_HK", back, sizeof(back)));
  close_fixture(&fixture);
  return failure;
}

static int us29_tc03_each_element_takes_the_default_and_can_be_set_alone(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  EXPECT(read_u64(&fixture, "COUNTED.items[0].level") == 9);
  EXPECT(read_u64(&fixture, "COUNTED.items[3].level") == 9);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "COUNTED.items[1].level"), 3) == FRAMEDSL_OK);
  /* A fixed member and a default in one byte: stamping the one leaves the other as it was set. */
  const uint8_t expected[] = { 0x43, 0x02, 0xA9, 0xA3 };
  EXPECT(encodes(fixture.frame, "COUNTED", expected, sizeof(expected)));
  EXPECT(encodes(fixture.frame, "COUNTED", expected, sizeof(expected)));
  close_fixture(&fixture);
  return failure;
}

static int us29_tc04_a_use_replaces_a_value_with_the_same_words(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  /* p: ?= over a default. q: := over a default. r: ?= over what the block fixed. */
  const uint8_t expected[] = { 0x41, 0x07, 0x02, 0x08, 0x02, 0x01, 0x09 };
  EXPECT(encodes(fixture.frame, "USES", expected, sizeof(expected)));

  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "USES.p.a"), 1) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "USES.q.a"), 1) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "USES.r.b"), 1) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "USES.p.b"), 1) == FRAMEDSL_ERROR_FIXED);
  close_fixture(&fixture);

  EXPECT(refused("block B { a: 8; };\npkt P { id: 8 := 1; bs: B[2] { a ?= 1; }; };\n", 33, ""));
  EXPECT(refused("block B { a: 8; };\npkt P { id: 8 := 1; bs: B[2] { a := 1; }; };\n", 33, ""));
  return failure;
}

static int us29_tc05_a_decode_reads_what_arrived(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t fault[] = { 0x40, 0x80, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01 };
  FramedslStatus status = FRAMEDSL_ERROR_UNRECOGNISED;
  const char *packet = framedsl_decode(fixture.frame, fault, sizeof(fault), NULL, &status);
  EXPECT(status == FRAMEDSL_OK && packet != NULL && strcmp(packet, "TM_HK") == 0);
  EXPECT(read_u64(&fixture, "TM_HK.spare") == 0x80);
  EXPECT(read_u64(&fixture, "TM_HK.temp") == 0);
  close_fixture(&fixture);
  return failure;
}

static int us29_tc06_a_conditional_default_follows_its_condition(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const FramedslFieldId on = id_of(&fixture, "OPTIONAL.on");

  const uint8_t without[] = { 0x42, 0x00, 0x00 };
  EXPECT(encodes(fixture.frame, "OPTIONAL", without, sizeof(without)));
  EXPECT(framedsl_set_u64(fixture.frame, on, 1) == FRAMEDSL_OK);
  const uint8_t with[] = { 0x42, 0x01, 0x55, 0x00 };
  EXPECT(encodes(fixture.frame, "OPTIONAL", with, sizeof(with)));

  /* A decode that carried another value, and then one that did not carry the field at all. */
  const uint8_t carried[] = { 0x42, 0x01, 0x66, 0x00 };
  FramedslStatus status = FRAMEDSL_ERROR_UNRECOGNISED;
  EXPECT(framedsl_decode(fixture.frame, carried, sizeof(carried), NULL, &status) != NULL);
  EXPECT(read_u64(&fixture, "OPTIONAL.opt") == 0x66);
  EXPECT(framedsl_decode(fixture.frame, without, sizeof(without), NULL, &status) != NULL);
  EXPECT(framedsl_set_u64(fixture.frame, on, 1) == FRAMEDSL_OK);
  EXPECT(encodes(fixture.frame, "OPTIONAL", with, sizeof(with)));
  close_fixture(&fixture);
  return failure;
}

static int us29_tc07_a_default_can_size_what_follows(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t two_elements[] = { 0x43, 0x02, 0xA9, 0xA9 };
  EXPECT(encodes(fixture.frame, "COUNTED", two_elements, sizeof(two_elements)));

  /* The length says two, and nothing has given the field two bytes. */
  FramedslStatus status = FRAMEDSL_OK;
  size_t length = 0;
  EXPECT(framedsl_encode(fixture.frame, "SIZED", &length, &status) == NULL && status == FRAMEDSL_ERROR_RANGE);

  const uint8_t payload[] = { 0xAA, 0xBB };
  EXPECT(framedsl_set_bytes(fixture.frame, id_of(&fixture, "SIZED.data"), payload, sizeof(payload)) == FRAMEDSL_OK);
  const uint8_t sized[] = { 0x44, 0x02, 0xAA, 0xBB };
  EXPECT(encodes(fixture.frame, "SIZED", sized, sizeof(sized)));
  close_fixture(&fixture);
  return failure;
}

/* A shorter repetition arriving leaves the elements past it as a frame starts, and not zero. */
static int us29_tc08_an_element_a_decode_did_not_carry_keeps_its_default(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t one[] = { 0x43, 0x01, 0xA5 };
  FramedslStatus status = FRAMEDSL_ERROR_UNRECOGNISED;
  EXPECT(framedsl_decode(fixture.frame, one, sizeof(one), NULL, &status) != NULL);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "COUNTED.count"), 3) == FRAMEDSL_OK);
  const uint8_t three[] = { 0x43, 0x03, 0xA5, 0xA9, 0xA9 };
  EXPECT(encodes(fixture.frame, "COUNTED", three, sizeof(three)));
  close_fixture(&fixture);
  return failure;
}

static int us29_tc09_a_default_identifies_nothing(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { x: 8 ?= 1; };\n", 11, "nothing identifies it"));
  EXPECT(refused("pkt A { id: 8 := 1; x: 8 ?= 1; };\npkt B { id: 8 := 1; x: 8 ?= 2; };\n", 12, ""));
  EXPECT(loads("pkt A { id: 8 := 1; x: 8 ?= 1; };\npkt B { id: 8 := 2; x: 8 ?= 1; };\n"));
  return failure;
}

static int us29_tc10_a_default_is_held_to_its_field(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { id: 8 := 1; x: 4 ?= 16; pad: 4; };\n", 8, "cannot hold the value"));
  EXPECT(refused("pkt A { id: 8 := 1; x: 128 ?= 0; };\n", 8, "bit string"));
  EXPECT(refused("pkt A { id: 8 := 1; x[4]: valof(id) * 8 ?= 0; };\n", 8, "bit string"));
  EXPECT(refused("enum E: 8 { A; };\npkt P { id: 8 := 1; e: E ?= B; };\n", 39, "has no name 'B'"));
  EXPECT(refused("enum E: 8 { A; };\npkt P { id: 8 ?= A; x: 8 := 1; };\n", 39, "not typed by an enum"));
  EXPECT(
    refused("enum E: 8 { A; };\nblock B { e: E; };\npkt P { id: 8 := 1; b: B { e ?= C; }; };\n", 39, "no name 'C'")
  );
  EXPECT(refused("block B { x: 8; };\npkt P { id: 8 := 1; b: B ?= 1; };\n", 0, "uses a block"));
  return failure;
}

static int us29_tc11_one_value_to_a_field(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { id: 8 := 1 ?= 2; };\n", 0, "expected ';'"));
  EXPECT(refused("pkt A { id: 8 ?= 1 := 2; };\n", 0, "expected ';'"));
  return failure;
}

static int us29_tc12_a_bare_equals_names_what_to_write(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { id: 8 = 1; };\n", 0, "':='"));
  EXPECT(refused("enum E: 8 { X; };\npkt A { id: 8 := 1; e: E = X; };\n", 0, "':='"));
  EXPECT(refused("block B { x: 8; };\npkt A { id: 8 := 1; b: B { x = 1; }; };\n", 0, "':='"));

  /* What `=` is still for. */
  EXPECT(loads("enum E: 8 { A = 7; B; };\ncal c(raw) = raw * 2 inverse(v) = v / 2;\npkt P { id: 8 := 1; };\n"));
  return failure;
}

static int us29_tc13_fixing_a_value_is_what_it_was(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(
    "enum E: 8 { A; B; };\n"
    "block H { id: 8; };\n"
    "pkt P { h: H { id := 0x10; }; e: E := B; n: 8 := 3; v: 8; };\n",
    "fixed.fdsl"
  );
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  int failure = frame == NULL ? __LINE__ : 0;
  const uint8_t expected[] = { 0x10, 0x01, 0x03, 0x00 };
  EXPECT(frame != NULL && encodes(frame, "P", expected, sizeof(expected)));
  FramedslStatus status = FRAMEDSL_ERROR_UNRECOGNISED;
  EXPECT(frame != NULL && framedsl_decode(frame, expected, sizeof(expected), NULL, &status) != NULL);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int us29_tc14_the_definition_reports_a_default(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  size_t packet = 0;
  size_t spare = 0;
  size_t identifier = 0;
  size_t power = 0;
  size_t items = 0;
  EXPECT(framedsl_find_packet(fixture.definition, "TM_HK", &packet));
  EXPECT(framedsl_find_field(fixture.definition, packet, "id", &identifier));
  EXPECT(framedsl_find_field(fixture.definition, packet, "spare", &spare));
  EXPECT(framedsl_find_field(fixture.definition, packet, "power", &power));

  FramedslField field;
  EXPECT(framedsl_field(fixture.definition, packet, identifier, &field));
  EXPECT(field.has_fixed && !field.has_default);
  EXPECT(framedsl_field(fixture.definition, packet, spare, &field));
  EXPECT(!field.has_fixed && field.has_default && field.default_value == 0);
  EXPECT(framedsl_field(fixture.definition, packet, power, &field));
  EXPECT(field.has_default && field.default_value == 1);

  EXPECT(framedsl_find_packet(fixture.definition, "COUNTED", &packet));
  EXPECT(framedsl_find_field(fixture.definition, packet, "items", &items));
  EXPECT(framedsl_member(fixture.definition, packet, items, 0, &field));
  EXPECT(field.has_fixed && !field.has_default);
  EXPECT(framedsl_member(fixture.definition, packet, items, 1, &field));
  EXPECT(!field.has_fixed && field.has_default && field.default_value == 9);
  close_fixture(&fixture);
  return failure;
}

/* A description says what a field defaults to, and `-d` says only what arrived. */
static int us29_tc15_the_tool_shows_a_default_and_never_compares_with_it(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/defaults.fdsl", args->tmp_dir);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return __LINE__;
  }
  fputs(DEFINITION, handle);
  fclose(handle);

  int failure = 0;
  char output[4096];
  char command[2048];
  snprintf(command, sizeof(command), "%s %s 2>&1", FRAMEDSL_BIN, path);
  FILE *pipe = popen(command, "r");
  size_t used = pipe != NULL ? fread(output, 1, sizeof(output) - 1, pipe) : 0;
  output[used] = '\0';
  if (pipe != NULL) {
    pclose(pipe);
  }
  EXPECT(strstr(output, "8 bits := 0x40") != NULL);
  EXPECT(strstr(output, "8 bits ?= 0x1 (OK)") != NULL);

  snprintf(command, sizeof(command), "%s %s -d '40 80 00 01 01 01 01 01' 2>&1", FRAMEDSL_BIN, path);
  pipe = popen(command, "r");
  used = pipe != NULL ? fread(output, 1, sizeof(output) - 1, pipe) : 0;
  output[used] = '\0';
  if (pipe != NULL) {
    pclose(pipe);
  }
  EXPECT(strstr(output, "    power          0 (0) = FAULT [Health]\n") != NULL);
  return failure;
}

void register_user_story29(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us29_tc01_nothing_set_encodes_the_defaults, "a field nobody set encodes its default", false);
  ADD_TEST_CASE(
    us29_tc02_setting_replaces_a_default_and_a_reset_brings_it_back,
    "a set replaces a default, a reset restores it",
    false
  );
  ADD_TEST_CASE(
    us29_tc03_each_element_takes_the_default_and_can_be_set_alone,
    "each element takes the default, and can differ",
    false
  );
  ADD_TEST_CASE(
    us29_tc04_a_use_replaces_a_value_with_the_same_words, "a use replaces a value in the same words", false
  );
  ADD_TEST_CASE(us29_tc05_a_decode_reads_what_arrived, "a decode replaces a default with what arrived", false);
  ADD_TEST_CASE(
    us29_tc06_a_conditional_default_follows_its_condition, "a default follows its field's condition", false
  );
  ADD_TEST_CASE(us29_tc07_a_default_can_size_what_follows, "a default sizes what follows, and may disagree", false);
  ADD_TEST_CASE(
    us29_tc08_an_element_a_decode_did_not_carry_keeps_its_default, "an element not carried reads its default", false
  );
  ADD_TEST_CASE(us29_tc09_a_default_identifies_nothing, "a default takes no part in checks 11 and 12", false);
  ADD_TEST_CASE(us29_tc10_a_default_is_held_to_its_field, "a default is held to check 8 and check 39", false);
  ADD_TEST_CASE(us29_tc11_one_value_to_a_field, "':=' and '?=' together do not parse", false);
  ADD_TEST_CASE(us29_tc12_a_bare_equals_names_what_to_write, "a bare '=' on a field is refused, naming ':='", false);
  ADD_TEST_CASE(us29_tc13_fixing_a_value_is_what_it_was, "':=' does what '=' did", false);
  ADD_TEST_CASE(us29_tc14_the_definition_reports_a_default, "a field and a member report a default", false);
  ADD_TEST_CASE(
    us29_tc15_the_tool_shows_a_default_and_never_compares_with_it, "the tool shows a default, and judges nothing", false
  );
}
