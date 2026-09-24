/* Story 10: composition, as somebody using the library and the tool sees it.
 *
 * The claim composition makes is that a packet built from blocks is the packet you would have written
 * by hand. So the first case holds the two side by side and compares bytes; the rest check that the
 * dotted names are how everything -- the API, the decoder, the terminal -- reaches a nested field. */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

static const char *const COMPOSED =
  "block Header { id: 8; sequence: 16; version: 4 = 0x1; flags: 4; };\n"
  "block Vector3 { x: 16; y: 16; z: 16; };\n"
  "pkt TM_IMU {\n"
  "  hdr:   Header { id = 0x83; flags = 0x3; };\n"
  "  accel: Vector3;\n"
  "  gyro:  Vector3;\n"
  "};\n"
  "pkt TM_ACK { hdr: Header { id = 0x81; }; result: 8; };\n";

static const char *const WRITTEN_OUT =
  "pkt TM_IMU {\n"
  "  id: 8 = 0x83; sequence: 16; version: 4 = 0x1; flags: 4 = 0x3;\n"
  "  ax: 16; ay: 16; az: 16;\n"
  "  gx: 16; gy: 16; gz: 16;\n"
  "};\n";

static bool set(const FramedslDefinition *definition, FramedslFrame *frame, const char *dotted, uint64_t value) {
  FramedslFieldId id;
  return framedsl_find(definition, dotted, &id) && framedsl_set_u64(frame, id, value) == FRAMEDSL_OK;
}

/* The same values through both definitions must give the same bytes. */
static int us10_tc01_a_composed_packet_is_the_packet_written_out(const TestArguments *args) {
  (void)args;
  FramedslResult *composed = framedsl_load_string(COMPOSED, "composed.fdsl");
  FramedslResult *written = framedsl_load_string(WRITTEN_OUT, "written.fdsl");
  const FramedslDefinition *c = framedsl_definition(composed);
  const FramedslDefinition *w = framedsl_definition(written);
  FramedslFrame *cf = framedsl_frame_new(c);
  FramedslFrame *wf = framedsl_frame_new(w);

  int failure = 0;
  if (cf == NULL || wf == NULL) {
    failure = __LINE__;
  } else if (!set(c, cf, "TM_IMU.hdr.sequence", 0x1234) || !set(c, cf, "TM_IMU.accel.x", 1)
             || !set(c, cf, "TM_IMU.gyro.z", 0xBEEF) || !set(w, wf, "TM_IMU.sequence", 0x1234)
             || !set(w, wf, "TM_IMU.ax", 1) || !set(w, wf, "TM_IMU.gz", 0xBEEF)) {
    failure = __LINE__;
  }

  size_t c_length = 0;
  size_t w_length = 0;
  const uint8_t *c_bytes = failure == 0 ? framedsl_encode(cf, "TM_IMU", &c_length, NULL) : NULL;
  uint8_t c_copy[64] = { 0 };
  if (c_bytes != NULL && c_length <= sizeof(c_copy)) {
    memcpy(c_copy, c_bytes, c_length);
  }
  const uint8_t *w_bytes = failure == 0 ? framedsl_encode(wf, "TM_IMU", &w_length, NULL) : NULL;

  if (
    failure == 0
    && (c_bytes == NULL || w_bytes == NULL || c_length != w_length || c_length != 16 || memcmp(c_copy, w_bytes, c_length) != 0 || c_copy[0] != 0x83 || c_copy[3] != 0x13)) {
    failure = __LINE__;
  }

  framedsl_frame_free(cf);
  framedsl_frame_free(wf);
  framedsl_result_free(composed);
  framedsl_result_free(written);
  return failure;
}

/* A shared header whose id each packet overrides is what tells them apart on the way in. */
static int us10_tc02_overrides_tell_packets_apart_on_decode(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(COMPOSED, "composed.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = framedsl_frame_new(definition);

  const uint8_t ack[] = { 0x81, 0x00, 0x07, 0x10, 0x2A };
  const char *name = frame == NULL ? NULL : framedsl_decode(frame, ack, sizeof(ack), NULL, NULL);

  FramedslFieldId sequence;
  FramedslFieldId result_id;
  uint64_t value = 0;
  int failure = 0;
  if (name == NULL || strcmp(name, "TM_ACK") != 0) {
    failure = __LINE__;
  } else if (!framedsl_find(definition, "TM_ACK.hdr.sequence", &sequence)
             || framedsl_get_u64(frame, sequence, &value) != FRAMEDSL_OK || value != 7) {
    failure = __LINE__;
  } else if (!framedsl_find(definition, "TM_ACK.result", &result_id)
             || framedsl_get_u64(frame, result_id, &value) != FRAMEDSL_OK || value != 42) {
    failure = __LINE__;
  }

  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* Introspection shows the expanded fields, with their full names and the overridden fixed values. */
static int us10_tc03_introspection_shows_the_expanded_fields(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(COMPOSED, "composed.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);

  size_t packet = 0;
  FramedslField first;
  FramedslField flags;
  FramedslField last;
  int failure = 0;
  if (!framedsl_find_packet(definition, "TM_IMU", &packet) || framedsl_field_count(definition, packet) != 10) {
    failure = __LINE__;
  } else if (!framedsl_field(definition, packet, 0, &first) || !framedsl_field(definition, packet, 3, &flags)
             || !framedsl_field(definition, packet, 9, &last)) {
    failure = __LINE__;
  } else if (strcmp(first.name, "hdr.id") != 0 || !first.has_fixed || first.fixed != 0x83
             || strcmp(flags.name, "hdr.flags") != 0 || flags.fixed != 0x3 || strcmp(last.name, "gyro.z") != 0
             || last.offset_bits != 112) {
    failure = __LINE__;
  }

  framedsl_result_free(result);
  return failure;
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

/* At the terminal: dotted names on the way in with -e, and on the way out with -d. */
static int us10_tc04_the_tool_takes_and_shows_dotted_names(const TestArguments *args) {
  (void)args;
  static char output[8192];
  char command[1024];

  /* hdr: 84 | 00 05 | version 2, flags 0 | axes 0001 0000 0000 | size 02 | AA BB */
  snprintf(
    command,
    sizeof(command),
    "%s %s/composed_packets.fdsl -e TM_SAMPLE hdr.sequence=5 sample.axes.x=1 sample.size=2 sample.extra=AABB -q "
    "2>&1",
    FRAMEDSL_BIN,
    EXAMPLES_DIR
  );
  if (run_capture(command, output, sizeof(output)) != 0
      || strcmp(output, "84 00 05 20 00 01 00 00 00 00 02 AA BB\n") != 0) {
    return __LINE__;
  }

  snprintf(
    command,
    sizeof(command),
    "%s %s/composed_packets.fdsl -d '84 00 05 20 00 01 00 00 00 00 02 AA BB' 2>&1",
    FRAMEDSL_BIN,
    EXAMPLES_DIR
  );
  if (run_capture(command, output, sizeof(output)) != 0 || strstr(output, "TM_SAMPLE") == NULL
      || strstr(output, "sample.axes.x") == NULL || strstr(output, "sample.extra") == NULL
      || strstr(output, "AA BB") == NULL) {
    return __LINE__;
  }

  /* The override is fixed, so -e refuses to set it like any other fixed field. */
  snprintf(command, sizeof(command), "%s %s/composed_packets.fdsl -e TM_ACK hdr.id=5 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR);
  if (run_capture(command, output, sizeof(output)) != 1 || strstr(output, "fixed") == NULL) {
    return __LINE__;
  }
  return 0;
}

/* #86: the named form keeps its instance in the name, the anonymous form does not have one -- and
 * each name is found only in the form it belongs to. The bytes are the same either way. */
static const char *const PASTED =
  "block Header { id: 8; sequence: 16; };\n"
  "pkt TM_ACK1 { header: Header { id = 0x81; }; result: 8; };\n"
  "pkt TM_ACK2 { Header { id = 0x82; }; result: 8; };\n";

static int us10_tc05_a_pasted_block_is_addressed_without_an_instance(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(PASTED, "pasted.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFieldId id;
  int failure = 0;

  if (!framedsl_ok(result)) {
    failure = __LINE__;
  } else if (!framedsl_find(definition, "TM_ACK1.header.sequence", &id)
             || framedsl_find(definition, "TM_ACK1.sequence", &id)) {
    failure = __LINE__;
  } else if (!framedsl_find(definition, "TM_ACK2.sequence", &id)
             || framedsl_find(definition, "TM_ACK2.header.sequence", &id)) {
    failure = __LINE__;
  }

  /* Decoding a TM_ACK2 reply, and reading it by the pasted name. */
  FramedslFrame *frame = failure == 0 ? framedsl_frame_new(definition) : NULL;
  const uint8_t reply[] = { 0x82, 0x00, 0x07, 0x01 };
  const char *name = frame == NULL ? NULL : framedsl_decode(frame, reply, sizeof(reply), NULL, NULL);
  uint64_t sequence = 0;
  if (
    failure == 0
    && (name == NULL || strcmp(name, "TM_ACK2") != 0 || !framedsl_find(definition, "TM_ACK2.sequence", &id) || framedsl_get_u64(frame, id, &sequence) != FRAMEDSL_OK || sequence != 7)) {
    failure = __LINE__;
  }

  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
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

/* An imported block pastes in like a local one, through a flat import and through `as`. */
static int us10_tc06_an_imported_block_can_be_pasted(const TestArguments *args) {
  if (!write_file(args, "link.fdsl", "block Header { id: 8; sequence: 16; };\n")
      || !write_file(
        args,
        "device.fdsl",
        "import \"link.fdsl\";\n"
        "import \"link.fdsl\" as link;\n"
        "pkt TM_A { Header { id = 0x01; }; tail: 8; };\n"
        "pkt TM_B { link.Header { id = 0x02; }; };\n"
      )) {
    FORCE_FAILURE();
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/device.fdsl", args->tmp_dir);
  FramedslResult *result = framedsl_load_file(path);
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFieldId id;
  int failure = 0;
  if (!framedsl_ok(result) || !framedsl_find(definition, "TM_A.sequence", &id)
      || !framedsl_find(definition, "TM_A.tail", &id) || !framedsl_find(definition, "TM_B.sequence", &id)
      || framedsl_find(definition, "TM_B.link.sequence", &id)) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  return failure;
}

void register_user_story10(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us10_tc01_a_composed_packet_is_the_packet_written_out, "a composed packet encodes as the packet written out", true
  );
  ADD_TEST_CASE(
    us10_tc02_overrides_tell_packets_apart_on_decode, "overridden ids tell packets sharing a header apart", false
  );
  ADD_TEST_CASE(
    us10_tc03_introspection_shows_the_expanded_fields, "introspection shows expanded names and fixed values", false
  );
  ADD_TEST_CASE(us10_tc04_the_tool_takes_and_shows_dotted_names, "-e and -d take and show dotted names", false);
  ADD_TEST_CASE(
    us10_tc05_a_pasted_block_is_addressed_without_an_instance,
    "a pasted block's fields have no instance in their names",
    false
  );
  ADD_TEST_CASE(us10_tc06_an_imported_block_can_be_pasted, "an imported block pastes in, flat or through 'as'", false);
}
