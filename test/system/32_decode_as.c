/* Story 32: a packet the caller names (#146).
 *
 * On CAN the identity is in the CAN ID, which the driver has and the payload does not. `@decode: manual`
 * says the program decides which packet arrived, so the definition needs nothing to identify one with,
 * and only size can deny the decode. */
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

static const char BUS[] =
  "@decode: manual;\n"
  "pkt ENGINE {\n"
  "  rpm:  16;\n"
  "  temp: 8;\n"
  "};\n"
  "pkt BATTERY {\n"
  "  volts: 16;\n"
  "  amps:  16;\n"
  "};\n"
  "pkt SIZED {\n"
  "  mark:    8 := 0x7E;\n"
  "  size:    8;\n"
  "  data[2]: valof(size) * 8;\n"
  "};\n"
  "[ @decode: auto; ]\n"
  "pkt PING {\n"
  "  id: 8 := 0x01;\n"
  "};\n";

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(Fixture *fixture) {
  fixture->result = framedsl_load_string(BUS, "bus.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void close_fixture(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static uint64_t read_u64(const Fixture *fixture, const char *dotted) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, dotted, &id);
  uint64_t value = UINT64_MAX;
  framedsl_get_u64(fixture->frame, id, &value);
  return value;
}

static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "loads.fdsl");
  const bool loaded = framedsl_ok(result);
  framedsl_result_free(result);
  return loaded;
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

static int us32_tc01_a_manual_packet_needs_nothing_to_identify_it(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(loads("[ @decode: manual; ]\npkt A { x: 8; };\n"));
  EXPECT(loads("[ @decode: manual; ]\npkt A { x: 8; };\n[ @decode: manual; ]\npkt B { x: 8; };\n"));
  EXPECT(loads("[ @decode: manual; ]\npkt A { id: 8 := 1; };\npkt B { id: 8 := 1; x: 8; };\n"));
  return failure;
}

static int us32_tc02_an_auto_packet_is_held_to_both_checks(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("pkt A { x: 8; };\n", 11, "nothing identifies it"));
  EXPECT(refused("[ @decode: auto; ]\npkt A { x: 8; };\n", 11, "nothing identifies it"));
  EXPECT(refused("@decode: manual;\n[ @decode: auto; ]\npkt A { x: 8; };\n", 11, "nothing identifies it"));
  EXPECT(refused("pkt A { id: 8 := 1; };\npkt B { id: 8 := 1; x: 8; };\n", 12, "cannot be told apart"));
  return failure;
}

static int us32_tc03_the_file_is_the_default_and_a_packet_the_exception(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  size_t index = 0;
  EXPECT(
    framedsl_find_packet(fixture.definition, "ENGINE", &index) && framedsl_packet_manual(fixture.definition, index)
  );
  EXPECT(
    framedsl_find_packet(fixture.definition, "PING", &index) && !framedsl_packet_manual(fixture.definition, index)
  );
  EXPECT(!framedsl_packet_manual(fixture.definition, 99));
  close_fixture(&fixture);
  return failure;
}

static int us32_tc04_it_goes_where_an_order_goes(const TestArguments *args) {
  (void)args;
  int failure = 0;
  EXPECT(refused("[ @decode: manual; ]\nblock B { x: 8; };\npkt P { id: 8 := 1; };\n", 0, "cannot apply to a block"));
  EXPECT(refused("pkt P { id: 8 := 1; [ @decode: manual; ] x: 8; };\n", 0, "cannot apply to a field"));
  EXPECT(refused("pkt P { id: 8 := 1; [ @decode: manual; ] block { x: 8; }; };\n", 0, "cannot apply to a block"));
  EXPECT(refused("@decode: maybe;\npkt P { id: 8 := 1; };\n", 0, "'decode' is 'auto' or 'manual', not 'maybe'"));
  EXPECT(loads("pkt P { decode: 8 := 1; };\n"));

  /* Written twice in one group, as an order can be: a warning, and the later one is in effect. */
  FramedslResult *twice =
    framedsl_load_string("[ @decode: manual; @decode: auto; ]\npkt P { id: 8 := 1; };\n", "t.fdsl");
  bool repeated = false;
  for (size_t i = 0; i < framedsl_diagnostic_count(twice); ++i) {
    repeated = repeated || framedsl_diagnostic(twice, i)->check == 34;
  }
  EXPECT(framedsl_ok(twice) && repeated && !framedsl_packet_manual(framedsl_definition(twice), 0));
  framedsl_result_free(twice);

  /* Saying what the file already said is a warning, and the packet still loads. */
  FramedslResult *result = framedsl_load_string("@decode: manual;\n[ @decode: manual; ]\npkt P { x: 8; };\n", "w.fdsl");
  bool warned = false;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    warned = warned || framedsl_diagnostic(result, i)->check == 31;
  }
  EXPECT(framedsl_ok(result) && warned);
  framedsl_result_free(result);
  return failure;
}

static int us32_tc05_it_reads_the_fields_and_says_what_it_took(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  /* Past the packet, the bytes are the caller's. */
  const uint8_t wire[] = { 0x01, 0x02, 0x03, 0xFF, 0xFF };
  size_t consumed = 0;
  EXPECT(framedsl_decode_as(fixture.frame, "ENGINE", wire, sizeof(wire), &consumed) == FRAMEDSL_OK);
  EXPECT(consumed == 3);
  EXPECT(read_u64(&fixture, "ENGINE.rpm") == 0x0102);
  EXPECT(read_u64(&fixture, "ENGINE.temp") == 3);
  EXPECT(framedsl_decode_as(fixture.frame, "BATTERY", wire, sizeof(wire), NULL) == FRAMEDSL_OK);
  EXPECT(read_u64(&fixture, "BATTERY.amps") == 0x03FF);
  close_fixture(&fixture);
  return failure;
}

static int us32_tc06_only_size_denies_it(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t good[] = { 0x7E, 0x01, 0xAA };
  EXPECT(framedsl_decode_as(fixture.frame, "SIZED", good, sizeof(good), NULL) == FRAMEDSL_OK);

  const uint8_t short_frame[] = { 0x7E, 0x02, 0xBB };
  EXPECT(
    framedsl_decode_as(fixture.frame, "SIZED", short_frame, sizeof(short_frame), NULL) == FRAMEDSL_ERROR_TRUNCATED
  );
  EXPECT(read_u64(&fixture, "SIZED.size") == 1);

  const uint8_t past[] = { 0x7E, 0x03, 0xBB, 0xCC, 0xDD };
  EXPECT(framedsl_decode_as(fixture.frame, "SIZED", past, sizeof(past), NULL) == FRAMEDSL_ERROR_RANGE);
  EXPECT(read_u64(&fixture, "SIZED.size") == 1);
  size_t length = 0;
  const uint8_t *bytes = NULL;
  FramedslFieldId data;
  EXPECT(framedsl_find(fixture.definition, "SIZED.data", &data));
  bytes = framedsl_get_bytes(fixture.frame, data, &length, NULL);
  EXPECT(bytes != NULL && length == 1 && bytes[0] == 0xAA);

  /* Empty, a packet with nothing to read past the end of. */
  EXPECT(framedsl_decode_as(fixture.frame, "ENGINE", good, 0, NULL) == FRAMEDSL_ERROR_TRUNCATED);
  close_fixture(&fixture);
  return failure;
}

static int us32_tc07_a_fixed_value_that_differs_reads_what_arrived(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0x7F, 0x00 };
  EXPECT(framedsl_decode_as(fixture.frame, "SIZED", wire, sizeof(wire), NULL) == FRAMEDSL_OK);
  EXPECT(read_u64(&fixture, "SIZED.mark") == 0x7F);
  close_fixture(&fixture);
  return failure;
}

static int us32_tc08_only_a_manual_packet_is_decoded_this_way(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0x01, 0x02, 0x03 };
  EXPECT(framedsl_decode_as(fixture.frame, "PING", wire, sizeof(wire), NULL) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_decode_as(fixture.frame, "NOPE", wire, sizeof(wire), NULL) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_decode_as(fixture.frame, NULL, wire, sizeof(wire), NULL) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_decode_as(NULL, "ENGINE", wire, sizeof(wire), NULL) == FRAMEDSL_ERROR_MEMORY);
  EXPECT(framedsl_decode_as(fixture.frame, "ENGINE", NULL, 0, NULL) == FRAMEDSL_ERROR_MEMORY);
  close_fixture(&fixture);
  return failure;
}

static int us32_tc09_framedsl_decode_never_returns_a_manual_packet(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  /* SIZED's fixed mark is in these bytes, and PING's id is not. */
  const uint8_t marked[] = { 0x7E, 0x00 };
  FramedslStatus status = FRAMEDSL_OK;
  EXPECT(framedsl_decode(fixture.frame, marked, sizeof(marked), NULL, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_UNRECOGNISED);

  const uint8_t ping[] = { 0x01 };
  const char *packet = framedsl_decode(fixture.frame, ping, sizeof(ping), NULL, &status);
  EXPECT(packet != NULL && strcmp(packet, "PING") == 0);
  close_fixture(&fixture);

  /* Nothing but manual packets, and more than the handful that are tried in turn. */
  FramedslResult *result = framedsl_load_string(
    "@decode: manual;\npkt A { x: 8; };\npkt B { x: 8; };\npkt C { x: 8; };\npkt D { x: 8; };\npkt E { x: 8; };\n",
    "all.fdsl"
  );
  FramedslFrame *frame = framedsl_frame_new(framedsl_definition(result));
  EXPECT(framedsl_decode(frame, marked, sizeof(marked), NULL, &status) == NULL);
  EXPECT(status == FRAMEDSL_ERROR_UNRECOGNISED);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int us32_tc10_encoding_gives_the_bare_payload(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    return __LINE__;
  }
  int failure = 0;
  FramedslFieldId rpm;
  EXPECT(framedsl_find(fixture.definition, "ENGINE.rpm", &rpm));
  EXPECT(framedsl_set_u64(fixture.frame, rpm, 0x0102) == FRAMEDSL_OK);
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture.frame, "ENGINE", &length, NULL);
  const uint8_t expected[] = { 0x01, 0x02, 0x00 };
  EXPECT(bytes != NULL && length == sizeof(expected) && memcmp(bytes, expected, length) == 0);

  bytes = framedsl_encode(fixture.frame, "SIZED", &length, NULL);
  const uint8_t fixed[] = { 0x7E, 0x00 };
  EXPECT(bytes != NULL && length == sizeof(fixed) && memcmp(bytes, fixed, length) == 0);
  close_fixture(&fixture);
  return failure;
}

/* The tool asks the same questions, with the mistakes in the command told apart from the frame's. */
static int us32_tc11_the_tool_decodes_a_packet_it_is_told(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/bus.fdsl", args->tmp_dir);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return __LINE__;
  }
  fputs(BUS, handle);
  fclose(handle);

  const struct {
    const char *arguments;
    int status;
    const char *text;
  } cases[] = {
    { "-d '01 02 03' --packet ENGINE", 0, "    rpm            258 (0x102)\n"                          },
    { "-d '01 02 03' --packet ENGINE", 0, "1 frame, 3 bytes\n"                                        },
    { "-d '01 02' --packet ENGINE",    1, "the bytes ran out"                                         },
    { "-d '01' --packet PING",         2, "'@decode: auto'"                                           },
    { "-d '01' --packet NOPE",         2, "no packet called 'NOPE'"                                   },
    { "--packet ENGINE",               2, "means nothing without it"                                  },
    { "",                              0, "pkt ENGINE: 2 fields, 24 bits reserved, @decode: manual\n" },
  };
  int failure = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char command[2048];
    snprintf(command, sizeof(command), "%s %s %s 2>&1", FRAMEDSL_BIN, path, cases[i].arguments);
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
      return __LINE__;
    }
    char output[4096];
    size_t used = fread(output, 1, sizeof(output) - 1, pipe);
    output[used] = '\0';
    const int closed = pclose(pipe);
    EXPECT(closed >> 8 == cases[i].status);
    EXPECT(strstr(output, cases[i].text) != NULL);
  }
  return failure;
}

void register_user_story32(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us32_tc01_a_manual_packet_needs_nothing_to_identify_it,
    "a manual packet needs no fixed field, and may overlap",
    false
  );
  ADD_TEST_CASE(us32_tc02_an_auto_packet_is_held_to_both_checks, "an auto packet is held to checks 11 and 12", false);
  ADD_TEST_CASE(
    us32_tc03_the_file_is_the_default_and_a_packet_the_exception,
    "the file is the default, a packet the exception",
    false
  );
  ADD_TEST_CASE(us32_tc04_it_goes_where_an_order_goes, "@decode goes on a file or a packet, and nowhere else", false);
  ADD_TEST_CASE(us32_tc05_it_reads_the_fields_and_says_what_it_took, "framedsl_decode_as() reads the fields", false);
  ADD_TEST_CASE(us32_tc06_only_size_denies_it, "only size denies it, and a failure changes nothing", false);
  ADD_TEST_CASE(
    us32_tc07_a_fixed_value_that_differs_reads_what_arrived, "a fixed value that differs reads what arrived", false
  );
  ADD_TEST_CASE(
    us32_tc08_only_a_manual_packet_is_decoded_this_way, "an auto packet or an unknown name is refused", false
  );
  ADD_TEST_CASE(
    us32_tc09_framedsl_decode_never_returns_a_manual_packet, "framedsl_decode() never returns a manual packet", false
  );
  ADD_TEST_CASE(us32_tc10_encoding_gives_the_bare_payload, "encoding a manual packet gives the bare payload", false);
  ADD_TEST_CASE(us32_tc11_the_tool_decodes_a_packet_it_is_told, "-d --packet decodes a manual packet only", false);
}
