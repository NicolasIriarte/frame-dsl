/* Story 14: warnings (#71) -- what loads fine and is almost certainly not what was meant.
 *
 * Check 30 is a declaration nothing uses, check 31 an attribute repeating what an enclosing scope
 * already wrote, check 34 an attribute written twice in one place (#119). All are warnings: the file loads, encodes and
 * decodes, and the warning is there to be read. Each case asserts where a warning is and that nothing else was said,
 * since a lint that over-reports is one people learn to ignore. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

static bool write_file(const TestArguments *args, const char *name, const char *content, char *path, size_t size) {
  snprintf(path, size, "%s/%s", args->tmp_dir, name);

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

/* The warnings a load produced, as "line:column:check" joined by spaces, in order -- so a case states
 * everything it expects in one string and anything extra fails it. Errors fail it outright. */
static bool warnings_of(FramedslResult *result, char *out, size_t size) {
  out[0] = '\0';
  if (!framedsl_ok(result)) {
    framedsl_result_free(result);
    return false;
  }
  size_t used = 0;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    if (diag->severity != FRAMEDSL_SEVERITY_WARNING) {
      framedsl_result_free(result);
      return false;
    }
    used += (size_t)snprintf(
      out + used, size - used, "%s%d:%d:%d", used == 0 ? "" : " ", diag->line, diag->column, diag->check
    );
  }
  framedsl_result_free(result);
  return true;
}

static int expect_warnings(const char *source, const char *expected) {
  char got[512];
  if (!warnings_of(framedsl_load_string(source, "t.fdsl"), got, sizeof(got))) {
    return __LINE__;
  }
  return strcmp(got, expected) == 0 ? 0 : __LINE__;
}

static int us14_tc01_an_unused_block_warns_at_its_name(const TestArguments *args) {
  (void)args;
  return expect_warnings(
    "block Spare {\n  s: 8;\n};\nblock Header {\n  id: 8 = 1;\n};\npkt TM {\n  h: Header;\n};\n", "1:7:30"
  );
}

static int us14_tc02_an_unused_calibration_warns(const TestArguments *args) {
  (void)args;
  int failure = expect_warnings("cal volts(x) = x * 2;\npkt TM {\n  id: 8 = 1;\n  raw: 8;\n};\n", "1:5:30");
  if (failure != 0) {
    return failure;
  }
  /* Used through @cal on a field, or only by being called from a curve that is used: both count. */
  return expect_warnings(
    "cal base(x) = x * 2;\ncal volts(x) = base(x) + 1;\npkt TM {\n  id: 8 = 1;\n  [ @cal: volts; ]\n  raw: 8;\n};\n", ""
  );
}

/* Uses count wherever they are written, even in something unused: so only the outermost unused
 * declaration of a chain is reported, and removing it is what brings the next one up. */
static int us14_tc03_only_the_outer_of_an_unused_chain_warns(const TestArguments *args) {
  (void)args;
  int failure = expect_warnings(
    "block Inner {\n  i: 8;\n};\nblock Outer {\n  o: Inner;\n};\npkt TM {\n  id: 8 = 1;\n  x: Outer;\n};\n", ""
  );
  if (failure != 0) {
    return failure;
  }
  return expect_warnings(
    "block Inner {\n  i: 8;\n};\nblock Outer {\n  o: Inner;\n};\npkt TM {\n  id: 8 = 1;\n};\n", "4:7:30"
  );
}

static int us14_tc04_an_unused_import_warns_at_the_import(const TestArguments *args) {
  char library[1024];
  if (!write_file(
        args, "lib/link.fdsl", "block Header {\n  id: 8 = 1;\n};\ncal volts(x) = x * 2;\n", library, sizeof(library)
      )) {
    return __LINE__;
  }
  const struct {
    const char *source;
    const char *expected;
  } cases[] = {
    { "import \"lib/link.fdsl\";\npkt TM {\n  id: 8 = 1;\n};\n",                                     "1:1:30" },
    { "import \"lib/link.fdsl\" as l;\npkt TM {\n  id: 8 = 1;\n};\n",                                "1:1:30" },
    { "import \"lib/link.fdsl\";\npkt TM {\n  h: Header;\n};\n",                                     ""       },
    { "import \"lib/link.fdsl\" as l;\npkt TM {\n  h: l.Header;\n};\n",                              ""       },
    /* Any one name is enough: the calibration alone, through the namespace. */
    { "import \"lib/link.fdsl\" as l;\npkt TM {\n  id: 8 = 1;\n  [ @cal: l.volts; ]\n  r: 8;\n};\n", ""       },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char top[1024];
    snprintf(top, sizeof(top), "%s/top.fdsl", args->tmp_dir);
    char got[512];
    if (!warnings_of(framedsl_load_source(cases[i].source, top), got, sizeof(got))
        || strcmp(got, cases[i].expected) != 0) {
      return __LINE__ * 100 + (int)i;
    }
  }
  return 0;
}

/* A file with no packets is a library: every declaration in it is an export, imports included. */
static int us14_tc05_a_library_file_does_not_warn_about_its_exports(const TestArguments *args) {
  char library[1024];
  if (!write_file(args, "lib/link.fdsl", "block Header {\n  id: 8 = 1;\n};\n", library, sizeof(library))) {
    return __LINE__;
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/shared.fdsl", args->tmp_dir);
  char got[512];
  if (!warnings_of(
        framedsl_load_source("import \"lib/link.fdsl\";\nblock Spare {\n  s: 8;\n};\ncal volts(x) = x * 2;\n", path),
        got,
        sizeof(got)
      )) {
    return __LINE__;
  }
  return got[0] == '\0' ? 0 : __LINE__;
}

/* An imported file's warnings are for whoever edits it; the importer does not repeat them. */
static int us14_tc06_an_imported_files_warnings_stay_there(const TestArguments *args) {
  char library[1024];
  char top[1024];
  if (!write_file(
        args,
        "lib/link.fdsl",
        "@byte_order: little_endian;\n[ @byte_order: little_endian; ]\nblock Header {\n  id: 8 = 1;\n};\n",
        library,
        sizeof(library)
      )
      || !write_file(args, "top.fdsl", "import \"lib/link.fdsl\";\npkt TM {\n  h: Header;\n};\n", top, sizeof(top))) {
    return __LINE__;
  }
  char got[512];
  if (!warnings_of(framedsl_load_file(library), got, sizeof(got)) || strcmp(got, "2:16:31") != 0) {
    return __LINE__;
  }
  if (!warnings_of(framedsl_load_file(top), got, sizeof(got))) {
    return __LINE__;
  }
  return got[0] == '\0' ? 0 : __LINE__;
}

/* The inner of two equal attributes warns, at its value, whatever scopes they are written at. */
static int us14_tc07_a_repeated_attribute_warns_at_every_scope(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    /* file, then packet */
    "@byte_order: little_endian;\n"
    "[ @byte_order: little_endian; ]\n"
    "pkt TM {\n  id: 8 = 1;\n};\n",
    /* packet, then field */
    "[ @bit_order: lsb_first; ]\n"
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @bit_order: lsb_first; ]\n"
    "  x: 8;\n};\n",
    /* packet, then block */
    "[ @byte_order: little_endian; ]\n"
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  block {\n    x: 16;\n  };\n};\n",
    /* block, then a field inside it */
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  block {\n"
    "    [ @byte_order: little_endian; ]\n"
    "    x: 16;\n  };\n};\n",
    /* file, then a named block's declaration */
    "@bit_order: lsb_first;\n"
    "[ @bit_order: lsb_first; ]\n"
    "block H {\n  id: 8 = 1;\n};\n"
    "pkt TM {\n  h: H;\n};\n",
  };
  const char *expected[] = { "2:16:31", "4:17:31", "4:18:31", "5:20:31", "2:15:31" };

  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    int failure = expect_warnings(sources[i], expected[i]);
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* Writing the language's default is documentation, not repetition; a different value is the point of
 * the attribute; and a sibling's attribute does not enclose anything. */
static int us14_tc08_what_is_not_a_repeated_attribute(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    /* the defaults, written out at file and packet scope */
    "@bit_order: msb_first;\n"
    "@byte_order: big_endian;\n"
    "pkt TM {\n  id: 8 = 1;\n};\n",
    "[ @bit_order: msb_first; ]\n"
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: big_endian; ]\n"
    "  x: 16;\n};\n",
    /* a value that changes something, and one that changes it back */
    "@byte_order: little_endian;\n"
    "[ @byte_order: big_endian; ]\n"
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  x: 16;\n};\n",
    /* two siblings saying the same thing: neither encloses the other */
    "pkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  x: 16;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  y: 16;\n};\n",
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    int failure = expect_warnings(sources[i], "");
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* A warning is advice: the file it is about works exactly as it would without it. */
static int us14_tc09_a_file_with_warnings_still_works(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(
    "@byte_order: little_endian;\n"
    "block Spare {\n  s: 8;\n};\n"
    "[ @byte_order: little_endian; ]\n"
    "pkt TM {\n  id: 8 = 0xA5;\n  value: 16;\n};\n",
    "t.fdsl"
  );
  int failure = 0;
  FramedslFrame *frame = NULL;
  if (!framedsl_ok(result) || framedsl_diagnostic_count(result) != 2) {
    failure = __LINE__;
  } else {
    frame = framedsl_frame_new(framedsl_definition(result));
    FramedslFieldId id;
    size_t length = 0;
    FramedslStatus status;
    if (frame == NULL || !framedsl_find(framedsl_definition(result), "TM.value", &id)
        || framedsl_set_u64(frame, id, 0x1234) != FRAMEDSL_OK) {
      failure = __LINE__;
    } else {
      const uint8_t *bytes = framedsl_encode(frame, "TM", &length, &status);
      uint8_t copy[3] = { 0 };
      if (bytes == NULL || length != 3) {
        failure = __LINE__;
      } else {
        memcpy(copy, bytes, 3);
        size_t consumed = 0;
        const char *name = framedsl_decode(frame, copy, 3, &consumed, &status);
        if (copy[0] != 0xA5 || copy[1] != 0x34 || copy[2] != 0x12 || name == NULL || strcmp(name, "TM") != 0) {
          failure = __LINE__;
        }
      }
    }
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

static int us14_tc10_every_example_is_warning_free(const TestArguments *args) {
  (void)args;
  const char *names[] = { "tm_datagram.fdsl",     "ccsds_space_packet.fdsl", "packet_family.fdsl",
                          "bit_order.fdsl",       "calibrated_sensor.fdsl",  "composed_packets.fdsl",
                          "imported_device.fdsl", "common/link.fdsl",        "common/sensors.fdsl",
                          "uart_device.fdsl" };
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, names[i]);
    char got[512];
    if (!warnings_of(framedsl_load_file(path), got, sizeof(got)) || got[0] != '\0') {
      return __LINE__ * 100 + (int)i;
    }
  }
  return 0;
}

/* The --check half of #70 that needed a warning to exist: one alone exits 0, and --werror makes it 1. */
static int us14_tc11_werror_decides_what_a_warning_costs(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "w.fdsl", "block Spare {\n  s: 8;\n};\npkt TM {\n  id: 8 = 1;\n};\n", path, sizeof(path))) {
    return __LINE__;
  }
  char expected[2800];
  snprintf(
    expected,
    sizeof(expected),
    "%s:1:7: warning: block 'Spare' is not used by anything [check 30]\n%s:1:1: note: remove the unused block "
    "'Spare'\n",
    path,
    path
  );

  for (int werror = 0; werror <= 1; ++werror) {
    char command[2400];
    snprintf(command, sizeof(command), "%s %s --check%s 2>/dev/null", FRAMEDSL_BIN, path, werror ? " --werror" : "");
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
      return __LINE__;
    }
    char output[2048] = { 0 };
    size_t used = fread(output, 1, sizeof(output) - 1, pipe);
    output[used] = '\0';
    int closed = pclose(pipe);
    int status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
    if (status != werror || strcmp(output, expected) != 0) {
      return __LINE__ * 10 + werror;
    }
  }
  return 0;
}

/* Check 34: the same attribute twice in one group, or in the file's defaults, warns at the later one's
 * name -- whatever the place, whichever the attribute, and whether or not the values agree. */
static int us14_tc12_an_attribute_written_twice_in_one_place_warns(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    /* a field's group, the same value */
    "pkt TM {\n  id: 8 = 1;\n  [ @bit_order: lsb_first; @bit_order: lsb_first; ]\n  x: 8;\n};\n",
    /* a packet's group, a different value */
    "[ @byte_order: little_endian; @byte_order: big_endian; ]\npkt TM {\n  id: 8 = 1;\n  x: 16;\n};\n",
    /* an anonymous block's group */
    "pkt TM {\n  id: 8 = 1;\n  [ @byte_order: little_endian; @byte_order: little_endian; ]\n"
    "  block {\n    x: 16;\n  };\n};\n",
    /* a named block's group, over several lines */
    "[\n  @bit_order: lsb_first;\n  @bit_order: msb_first;\n]\nblock H {\n  id: 8 = 1;\n};\npkt TM {\n  h: H;\n};\n",
    /* a calibration, each of the two used somewhere so that check 30 has nothing to say */
    "cal c(x) = x * 2;\ncal k(x) = x * 3;\npkt TM {\n  id: 8 = 1;\n  [ @cal: k; @cal: c; ]\n  x: 8;\n"
    "  [ @cal: k; ]\n  y: 8;\n};\n",
    /* the file's defaults */
    "@byte_order: little_endian;\n@bit_order: lsb_first;\n@byte_order: little_endian;\npkt TM {\n  id: 8 = 1;\n};\n",
  };
  const char *expected[] = { "3:29:34", "1:32:34", "3:34:34", "3:4:34", "5:15:34", "3:2:34" };

  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    int failure = expect_warnings(sources[i], expected[i]);
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* The message says which case it is, and the file means what it did before the warning existed: the
 * last one written wins. */
static int us14_tc13_the_last_of_two_still_wins(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(
    "cal c(x) = x * 2;\ncal k(x) = x * 3;\npkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: little_endian; @byte_order: big_endian; @cal: k; @cal: c; ]\n  x: 16;\n"
    "  [ @bit_order: lsb_first; @bit_order: lsb_first; @cal: k; ]\n  y: 8;\n};\n",
    "t.fdsl"
  );
  const char *messages[] = {
    "'@byte_order' is written twice: this 'big_endian' overrides the 'little_endian' on line 5",
    "'@cal' is written twice: this 'c' overrides the 'k' on line 5",
    "'@bit_order: lsb_first' is written twice; the one on line 7 already says it",
  };
  int failure = 0;
  FramedslField field;
  size_t packet = 0;
  if (!framedsl_ok(result) || framedsl_diagnostic_count(result) != 3) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < 3 && failure == 0; ++i) {
      const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
      if (diag->check != 34 || strcmp(diag->message, messages[i]) != 0) {
        failure = __LINE__ * 10 + (int)i;
      }
    }
  }
  if (failure == 0
      && (!framedsl_find_packet(framedsl_definition(result), "TM", &packet)
          || !framedsl_field(framedsl_definition(result), packet, 1, &field)
          || field.byte_order != FRAMEDSL_BYTE_ORDER_BIG_ENDIAN || field.cal == NULL || strcmp(field.cal, "c") != 0)) {
    failure = __LINE__;
  }
  if (
    failure == 0
    && (!framedsl_field(framedsl_definition(result), packet, 2, &field) || field.bit_order != FRAMEDSL_BIT_ORDER_LSB_FIRST)) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  return failure;
}

/* Different attributes in one group, and one attribute in two groups, are not written twice in one
 * place. The second of these is check 31's when one group encloses the other, and nothing when not. */
static int us14_tc14_what_is_not_written_twice(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    "cal c(x) = x * 2;\npkt TM {\n  id: 8 = 1;\n"
    "  [ @bit_order: lsb_first; @byte_order: little_endian; @cal: c; ]\n  x: 16;\n};\n",
    "cal c(x) = x * 2;\n[ @byte_order: little_endian; ]\npkt TM {\n  id: 8 = 1;\n"
    "  [ @byte_order: big_endian; @cal: c; ]\n  x: 16;\n  [ @cal: c; ]\n  y: 8;\n};\n",
    "@bit_order: lsb_first;\n[ @byte_order: little_endian; ]\npkt TM {\n  id: 8 = 1;\n};\n",
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    int failure = expect_warnings(sources[i], "");
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* --check prints check 34 like any warning, and --werror refuses the file for it. */
static int us14_tc15_werror_refuses_an_attribute_written_twice(const TestArguments *args) {
  char path[1024];
  if (!write_file(
        args,
        "twice.fdsl",
        "pkt TM {\n  id: 8 = 1;\n  [ @bit_order: lsb_first; @bit_order: msb_first; ]\n  x: 8;\n};\n",
        path,
        sizeof(path)
      )) {
    return __LINE__;
  }
  char expected[2800];
  snprintf(
    expected,
    sizeof(expected),
    "%s:3:29: warning: '@bit_order' is written twice: this 'msb_first' overrides the 'lsb_first' on line 3 "
    "[check 34]\n%s:3:5: note: remove the earlier '@bit_order'\n",
    path,
    path
  );

  for (int werror = 0; werror <= 1; ++werror) {
    char command[2400];
    snprintf(command, sizeof(command), "%s %s --check%s 2>/dev/null", FRAMEDSL_BIN, path, werror ? " --werror" : "");
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
      return __LINE__;
    }
    char output[2048] = { 0 };
    size_t used = fread(output, 1, sizeof(output) - 1, pipe);
    output[used] = '\0';
    int closed = pclose(pipe);
    int status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
    if (status != werror || strcmp(output, expected) != 0) {
      return __LINE__ * 10 + werror;
    }
  }
  return 0;
}

void register_user_story14(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us14_tc01_an_unused_block_warns_at_its_name, "an unused block warns at its name", false);
  ADD_TEST_CASE(us14_tc02_an_unused_calibration_warns, "an unused calibration warns; a called one does not", false);
  ADD_TEST_CASE(us14_tc03_only_the_outer_of_an_unused_chain_warns, "only the outer of an unused chain warns", false);
  ADD_TEST_CASE(us14_tc04_an_unused_import_warns_at_the_import, "an import nothing uses warns, flat or 'as'", false);
  ADD_TEST_CASE(
    us14_tc05_a_library_file_does_not_warn_about_its_exports, "a file with no packets is a library, and quiet", false
  );
  ADD_TEST_CASE(us14_tc06_an_imported_files_warnings_stay_there, "an imported file's warnings stay in it", false);
  ADD_TEST_CASE(
    us14_tc07_a_repeated_attribute_warns_at_every_scope, "a repeated attribute warns at every scope", false
  );
  ADD_TEST_CASE(us14_tc08_what_is_not_a_repeated_attribute, "defaults, changes and siblings are not repeats", false);
  ADD_TEST_CASE(us14_tc09_a_file_with_warnings_still_works, "a file with warnings loads, encodes and decodes", false);
  ADD_TEST_CASE(us14_tc10_every_example_is_warning_free, "every example is warning-free", false);
  ADD_TEST_CASE(us14_tc11_werror_decides_what_a_warning_costs, "--check exits 0 on a warning, 1 with --werror", false);
  ADD_TEST_CASE(
    us14_tc12_an_attribute_written_twice_in_one_place_warns, "an attribute written twice in one place warns", false
  );
  ADD_TEST_CASE(us14_tc13_the_last_of_two_still_wins, "of an attribute written twice, the last still wins", false);
  ADD_TEST_CASE(us14_tc14_what_is_not_written_twice, "other attributes, and other groups, are not twice", false);
  ADD_TEST_CASE(
    us14_tc15_werror_refuses_an_attribute_written_twice, "--werror refuses an attribute written twice", false
  );
}
