/* Story 11: imports, on a real file system.
 *
 * Each case writes a small tree of definition files into its own empty directory and loads the top
 * one with framedsl_load_file(), which is how a program meets imports: paths relative to the file,
 * files that may not exist, a library reached twice. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

static bool write_file(const TestArguments *args, const char *name, const char *content) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, name);

  /* One level of subdirectory is all any case needs. */
  char directory[1024];
  snprintf(directory, sizeof(directory), "%s", path);
  char *slash = strrchr(directory, '/');
  if (slash != NULL && slash > directory + strlen(args->tmp_dir)) {
    *slash = '\0';
    mkdir(directory, 0755);
  }

  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static FramedslResult *load(const TestArguments *args, const char *name) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, name);
  return framedsl_load_file(path);
}

/* Whether the load failed with check `wanted` among its diagnostics. */
static bool rejected_by(FramedslResult *result, int wanted) {
  if (framedsl_ok(result)) {
    return false;
  }
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    if (framedsl_diagnostic(result, i)->check == wanted) {
      return true;
    }
  }
  return false;
}

static const char *const LINK =
  "@byte_order: big_endian;\n"
  "block Header { id: 8; sequence: 16; };\n"
  "cal volts(raw) = raw * 3.3 / 4095;\n"
  "pkt LINK_ONLY { magic: 8 = 0xEE; };\n";

static int us11_tc01_a_flat_import_is_used_as_if_written_here(const TestArguments *args) {
  if (!write_file(args, "lib/link.fdsl", LINK)
      || !write_file(
        args,
        "device.fdsl",
        "import \"lib/link.fdsl\";\n"
        "pkt TM { hdr: Header { id = 0x82; }; [ @cal: volts; ] bus: 16; };\n"
      )) {
    FORCE_FAILURE();
  }

  FramedslResult *result = load(args, "device.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFieldId id;
  int failure = 0;
  if (!framedsl_ok(result) || !framedsl_find(definition, "TM.hdr.sequence", &id)
      || !framedsl_find_cal(definition, "volts", NULL)) {
    failure = __LINE__;
  } else if (framedsl_packet_count(definition) != 1 || framedsl_find_packet(definition, "LINK_ONLY", NULL)) {
    /* The library's packet is checked, not imported: importing never changes what a decoder matches. */
    failure = __LINE__;
  }
  framedsl_result_free(result);
  return failure;
}

/* `as` names everything the file brings, and only under that name. */
static int us11_tc02_an_import_as_a_name_is_addressed_through_it(const TestArguments *args) {
  if (!write_file(args, "link.fdsl", LINK)
      || !write_file(
        args,
        "device.fdsl",
        "import \"link.fdsl\" as link;\n"
        "pkt TM { hdr: link.Header { id = 0x82; }; [ @cal: link.volts; ] bus: 16; };\n"
      )
      || !write_file(args, "flat.fdsl", "import \"link.fdsl\" as link;\npkt TM { hdr: Header { id = 1; }; };\n")) {
    FORCE_FAILURE();
  }

  FramedslResult *result = load(args, "device.fdsl");
  FramedslResult *flat = load(args, "flat.fdsl");
  int failure = 0;
  if (!framedsl_ok(result) || !framedsl_find_cal(framedsl_definition(result), "link.volts", NULL)) {
    failure = __LINE__;
  } else if (!rejected_by(flat, 23)) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  framedsl_result_free(flat);
  return failure;
}

/* Two libraries importing the same third one, and a file importing both: the shared declarations
 * arrive twice and are the same declarations, so nothing clashes. */
static int us11_tc03_a_file_reached_twice_is_merged_once(const TestArguments *args) {
  if (!write_file(args, "base.fdsl", "block Crc { crc: 16; };\n")
      || !write_file(args, "a.fdsl", "import \"base.fdsl\";\nblock A { a: 8; };\n")
      || !write_file(args, "b.fdsl", "import \"base.fdsl\";\nblock B { b: 8; };\n")
      || !write_file(
        args,
        "top.fdsl",
        "import \"a.fdsl\";\nimport \"b.fdsl\";\nimport \"base.fdsl\";\n"
        "pkt P { id: 8 = 1; x: A; y: B; end: Crc; };\n"
      )) {
    FORCE_FAILURE();
  }
  FramedslResult *result = load(args, "top.fdsl");
  int failure = framedsl_ok(result) ? 0 : __LINE__;
  framedsl_result_free(result);
  return failure;
}

/* Two different declarations of one name are a clash; `as` on one of them keeps both. */
static int us11_tc04_different_declarations_of_one_name_clash(const TestArguments *args) {
  if (!write_file(args, "one.fdsl", "block Header { id: 8; };\n")
      || !write_file(args, "two.fdsl", "block Header { kind: 8; };\n")
      || !write_file(
        args, "clash.fdsl", "import \"one.fdsl\";\nimport \"two.fdsl\";\npkt P { h: Header { id = 1; }; };\n"
      )
      || !write_file(
        args,
        "both.fdsl",
        "import \"one.fdsl\";\nimport \"two.fdsl\" as two;\npkt P { h: Header { id = 1; }; k: two.Header; };\n"
      )) {
    FORCE_FAILURE();
  }
  FramedslResult *clash = load(args, "clash.fdsl");
  FramedslResult *both = load(args, "both.fdsl");
  int failure = 0;
  if (!rejected_by(clash, 28)) {
    failure = __LINE__;
  } else if (!framedsl_ok(both)) {
    failure = __LINE__;
  }
  framedsl_result_free(clash);
  framedsl_result_free(both);
  return failure;
}

static int us11_tc05_a_missing_file_and_a_cycle_are_refused(const TestArguments *args) {
  if (!write_file(args, "missing.fdsl", "import \"nowhere.fdsl\";\npkt P { id: 8 = 1; };\n")
      || !write_file(args, "a.fdsl", "import \"b.fdsl\";\nblock A { a: 8; };\npkt P { id: 8 = 1; };\n")
      || !write_file(args, "b.fdsl", "import \"a.fdsl\";\nblock B { b: 8; };\n")
      || !write_file(args, "self.fdsl", "import \"self.fdsl\";\npkt P { id: 8 = 1; };\n")) {
    FORCE_FAILURE();
  }
  const char *names[] = { "missing.fdsl", "a.fdsl", "self.fdsl" };
  const int checks[] = { 26, 27, 27 };
  for (size_t i = 0; i < 3; ++i) {
    FramedslResult *result = load(args, names[i]);
    bool ok = rejected_by(result, checks[i]);
    framedsl_result_free(result);
    if (!ok) {
      return __LINE__;
    }
  }
  return 0;
}

/* A mistake inside an imported file is reported in that file, at its own position -- not at the
 * import, and not as a position in the file that imported it. */
static int us11_tc06_an_error_in_an_imported_file_names_that_file(const TestArguments *args) {
  if (!write_file(args, "lib/bad.fdsl", "block Good { g: 8; };\nblock Bad {\n  x: 4 - 4;\n};\n")
      || !write_file(args, "device.fdsl", "import \"lib/bad.fdsl\";\npkt P { id: 8 = 1; };\n")) {
    FORCE_FAILURE();
  }
  FramedslResult *result = load(args, "device.fdsl");
  int failure = __LINE__;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    if (diag->check == 4 && diag->file != NULL && strstr(diag->file, "lib/bad.fdsl") != NULL && diag->line == 3) {
      failure = 0;
    }
  }
  framedsl_result_free(result);
  return failure;
}

/* An imported block's layout was decided in its own file. Here the device is little-endian and the
 * header big-endian, and each field goes out in its own file's order. */
static int us11_tc07_an_imported_block_keeps_its_own_byte_order(const TestArguments *args) {
  if (!write_file(args, "link.fdsl", LINK)
      || !write_file(
        args,
        "device.fdsl",
        "import \"link.fdsl\";\n@byte_order: little_endian;\npkt TM { hdr: Header { id = 0x82; }; value: 16; };\n"
      )) {
    FORCE_FAILURE();
  }
  FramedslResult *result = load(args, "device.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = framedsl_frame_new(definition);
  FramedslFieldId sequence;
  FramedslFieldId value;

  int failure = 0;
  size_t length = 0;
  const uint8_t expected[] = { 0x82, 0x12, 0x34, 0x34, 0x12 };
  if (frame == NULL || !framedsl_find(definition, "TM.hdr.sequence", &sequence)
      || !framedsl_find(definition, "TM.value", &value) || framedsl_set_u64(frame, sequence, 0x1234) != FRAMEDSL_OK
      || framedsl_set_u64(frame, value, 0x1234) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    const uint8_t *bytes = framedsl_encode(frame, "TM", &length, NULL);
    if (bytes == NULL || length != sizeof(expected) || memcmp(bytes, expected, length) != 0) {
      failure = __LINE__;
    }
  }
  framedsl_frame_free(frame);
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

/* At the terminal: the shipped example loads and encodes, an imported file's error is shown with its
 * own name, and --emit-c turns a qualified calibration into a C name. A string loaded with
 * framedsl_load_string() resolves its imports from the working directory, which for these tests is
 * the repository. */
static int us11_tc08_the_tool_and_a_string_load_follow_imports(const TestArguments *args) {
  (void)args;
  static char output[8192];
  char command[1024];

  snprintf(
    command,
    sizeof(command),
    "%s %s/imported_device.fdsl -e TM_TEMPERATURE hdr.sequence=1 board=800 supply=2048 end.crc=0xBEEF -q 2>&1",
    FRAMEDSL_BIN,
    EXAMPLES_DIR
  );
  if (run_capture(command, output, sizeof(output)) != 0 || strcmp(output, "82 00 01 10 20 03 00 08 BE EF\n") != 0) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s/imported_device.fdsl --emit-c 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR);
  if (run_capture(command, output, sizeof(output)) != 0 || strstr(output, "fdsl_sensors_celsius(double raw)") == NULL) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s/invalid/import_cycle.fdsl 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR);
  if (run_capture(command, output, sizeof(output)) != 1 || strstr(output, "import_cycle_back.fdsl:3:1:") == NULL) {
    return __LINE__;
  }

  FramedslResult *result = framedsl_load_string(
    "import \"" EXAMPLES_DIR "/common/link.fdsl\";\npkt P { h: Header { id = 1; }; };\n", "string"
  );
  bool ok = framedsl_ok(result);
  framedsl_result_free(result);
  return ok ? 0 : __LINE__;
}

void register_user_story11(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us11_tc01_a_flat_import_is_used_as_if_written_here, "a flat import is used as if written here, packets aside", true
  );
  ADD_TEST_CASE(
    us11_tc02_an_import_as_a_name_is_addressed_through_it, "an import 'as' a name is reached only through it", false
  );
  ADD_TEST_CASE(us11_tc03_a_file_reached_twice_is_merged_once, "a file reached twice is merged once", false);
  ADD_TEST_CASE(
    us11_tc04_different_declarations_of_one_name_clash, "two declarations of one name clash; 'as' keeps both", false
  );
  ADD_TEST_CASE(us11_tc05_a_missing_file_and_a_cycle_are_refused, "a missing file and a cycle are refused", false);
  ADD_TEST_CASE(
    us11_tc06_an_error_in_an_imported_file_names_that_file, "an error in an imported file names that file", false
  );
  ADD_TEST_CASE(
    us11_tc07_an_imported_block_keeps_its_own_byte_order, "an imported block keeps its own byte order", false
  );
  ADD_TEST_CASE(
    us11_tc08_the_tool_and_a_string_load_follow_imports, "the tool and a string load follow imports", false
  );
}
