/* Story 15: the formatter (#29).
 *
 * One layout, no options. Each rule has a case with the text going in and the exact text coming out;
 * then the properties that make a formatter safe to run on save -- it is idempotent, it keeps every
 * comment and every number's spelling, the result means what the original meant, and a file that does
 * not parse is left alone. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

static int expect_format(const char *source, const char *expected) {
  char *out = NULL;
  if (framedsl_format(source, &out) != FRAMEDSL_OK) {
    return __LINE__;
  }
  int failure = strcmp(out, expected) == 0 ? 0 : __LINE__;
  if (failure != 0) {
    fprintf(stderr, "--- expected\n%s--- got\n%s---\n", expected, out);
  }
  free(out);
  return failure;
}

/* Two spaces a level, never a tab, and a single space where the language needs one. */
static int us15_tc01_indentation_and_spacing(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt\tTM{\n\tid:8=0x1;\n    block{\n  x:valof( id )*8 - 32;\n};\n};\n",
    "pkt TM {\n  id: 8 = 0x1;\n  block {\n    x: valof(id) * 8 - 32;\n  };\n};\n"
  );
}

/* Widths align within a run of fields: comments, groups and blank lines do not end it, a nested block
 * does. The name keeps its colon; the padding is after it. */
static int us15_tc02_fields_align_within_a_run(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n"
    "  id: 8 = 1;\n"
    "  # a comment\n"
    "  sequence: 16;\n"
    "\n"
    "  [ @byte_order: little_endian; ]\n"
    "  data[4]: valof(sequence);\n"
    "  block {\n"
    "    a: 8;\n"
    "    bb: 8;\n"
    "  };\n"
    "  crc: 16;\n"
    "  x: 8;\n"
    "};\n",
    "pkt TM {\n"
    "  id:       8 = 1;\n"
    "  # a comment\n"
    "  sequence: 16;\n"
    "\n"
    "  [ @byte_order: little_endian; ]\n"
    "  data[4]:  valof(sequence);\n"
    "  block {\n"
    "    a:  8;\n"
    "    bb: 8;\n"
    "  };\n"
    "  crc: 16;\n"
    "  x:   8;\n"
    "};\n"
  );
}

/* Trailing comments start in one column within a run, two spaces past its longest line. */
static int us15_tc03_trailing_comments_align(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n  id: 8 = 1; # the id\n  sequence: 16;\n  flags: 8;     # flags\n};\n",
    "pkt TM {\n  id:       8 = 1;  # the id\n  sequence: 16;\n  flags:    8;      # flags\n};\n"
  );
}

/* One setting stays on one line; more are one a line, aligned on their values. File settings align
 * the same way. */
static int us15_tc04_attribute_groups(const TestArguments *args) {
  (void)args;
  return expect_format(
    "@bit_order: msb_first;\n@byte_order:little_endian;\n"
    "[@byte_order:big_endian;]\npkt TM {\n  id: 8 = 1;\n  [ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "  x: 16;\n};\n",
    "@bit_order:  msb_first;\n@byte_order: little_endian;\n\n"
    "[ @byte_order: big_endian; ]\npkt TM {\n  id: 8 = 1;\n  [\n    @bit_order:  lsb_first;\n"
    "    @byte_order: little_endian;\n  ]\n  x:  16;\n};\n"
  );
}

/* At most one blank line; none just inside braces; exactly one between top-level declarations, with a
 * comment in front of one staying with it; one newline at the end. */
static int us15_tc05_blank_lines(const TestArguments *args) {
  (void)args;
  return expect_format(
    "\n\n# header\n\n\n\ncal a(x) = x * 2;\ncal b(x) = x * 3;\n# about TM\npkt TM {\n\n  id: 8 = 1;\n\n\n\n  x: "
    "8;\n\n};"
    "\n\n\n",
    "# header\n\ncal a(x) = x * 2;\n\ncal b(x) = x * 3;\n\n# about TM\npkt TM {\n  id: 8 = 1;\n\n  x:  8;\n};\n"
  );
}

/* Numbers keep their spelling; imports, uses with overrides and calibrations get their spacing. */
static int us15_tc06_spelling_and_the_other_declarations(const TestArguments *args) {
  (void)args;
  int failure = expect_format(
    "block H {\n  id: 0b1000 = 0B0001;\n};\n"
    "pkt TM {\n  h : H{id=0xA;};\n  v: 0x10 = 0017;\n};\n",
    "block H {\n  id: 0b1000 = 0B0001;\n};\n\npkt TM {\n  h: H { id = 0xA; };\n  v: 0x10 = 0017;\n};\n"
  );
  if (failure != 0) {
    return failure;
  }
  return expect_format(
    "import   \"lib/a.fdsl\"  as  a ;\nimport \"lib/b.fdsl\";\n"
    "cal  f( x )=-x*2.5e-1+( 3-x )/4 inverse(y)= y ;\n",
    "import \"lib/a.fdsl\" as a;\nimport \"lib/b.fdsl\";\n\ncal f(x) = -x * 2.5e-1 + (3 - x) / 4 inverse (y) = y;\n"
  );
}

/* A comment in the middle of a declaration is kept where it was, and the declaration goes on on the
 * next line rather than the comment moving. */
static int us15_tc07_every_comment_survives(const TestArguments *args) {
  (void)args;
  const char *source =
    "# first\n"
    "pkt TM { # opening\n"
    "  id: 8 # mid-field\n"
    "    = 1;\n"
    "  [ # after bracket\n"
    "    @byte_order: little_endian;\n"
    "    # before close bracket\n"
    "  ]\n"
    "  v: 16;\n"
    "  # before close brace\n"
    "}; # after close\n"
    "# last";
  char *out = NULL;
  if (framedsl_format(source, &out) != FRAMEDSL_OK) {
    return __LINE__;
  }
  const char *comments[] = {
    "# first",       "# opening", "# mid-field", "# after bracket", "# before close bracket", "# before close brace",
    "# after close", "# last"
  };
  int failure = 0;
  const char *from = out;
  for (size_t i = 0; i < sizeof(comments) / sizeof(*comments) && failure == 0; ++i) {
    const char *at = strstr(from, comments[i]);
    if (at == NULL) {
      failure = __LINE__ * 100 + (int)i;
    } else {
      from = at; /* and in the same order */
    }
  }
  free(out);
  return failure;
}

static int us15_tc08_no_tab_survives(const TestArguments *args) {
  (void)args;
  char *out = NULL;
  if (framedsl_format("\tpkt\tTM\t{\n\t\tid:\t8\t=\t1;\n\t};\n", &out) != FRAMEDSL_OK) {
    return __LINE__;
  }
  int failure = strchr(out, '\t') == NULL ? 0 : __LINE__;
  free(out);
  return failure;
}

static int us15_tc09_a_file_that_does_not_parse_is_refused(const TestArguments *args) {
  (void)args;
  char *out = NULL;
  if (framedsl_format("pkt TM {\n  id: 8\n};\n", &out) != FRAMEDSL_ERROR_INVALID || out != NULL) {
    return __LINE__;
  }
  /* Only syntax: a file that parses and fails a check is formatted like any other. */
  if (framedsl_format("pkt TM {\n  x:    3;\n};\n", &out) != FRAMEDSL_OK) {
    return __LINE__;
  }
  free(out);
  return 0;
}

/* ---- Every definition file in the repository ----------------------------------------------------- */

static const char *const FILES[] = {
  "examples/bit_order.fdsl",
  "examples/calibrated_sensor.fdsl",
  "examples/ccsds_space_packet.fdsl",
  "examples/composed_packets.fdsl",
  "examples/imported_device.fdsl",
  "examples/packet_family.fdsl",
  "examples/tm_datagram.fdsl",
  "examples/uart_device.fdsl",
  "examples/common/link.fdsl",
  "examples/common/sensors.fdsl",
  "test/corpus/bit_order.fdsl",
  "test/corpus/bit_string.fdsl",
  "test/corpus/bit_string_offset.fdsl",
  "test/corpus/byte_aligned.fdsl",
  "test/corpus/byte_order.fdsl",
  "test/corpus/family.fdsl",
  "test/corpus/fixed_over.fdsl",
  "test/corpus/sub_byte.fdsl",
  "test/corpus/variable.fdsl",
};

static char *read_file(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    return NULL;
  }
  static char buffer[1 << 16];
  size_t used = fread(buffer, 1, sizeof(buffer) - 1, handle);
  fclose(handle);
  buffer[used] = '\0';
  char *copy = malloc(used + 1);
  if (copy != NULL) {
    memcpy(copy, buffer, used + 1);
  }
  return copy;
}

static int us15_tc10_formatting_twice_changes_nothing(const TestArguments *args) {
  (void)args;
  for (size_t i = 0; i < sizeof(FILES) / sizeof(*FILES); ++i) {
    char *source = read_file(FILES[i]);
    char *once = NULL;
    char *twice = NULL;
    int failure = 0;
    if (source == NULL || framedsl_format(source, &once) != FRAMEDSL_OK || framedsl_format(once, &twice) != FRAMEDSL_OK
        || strcmp(once, twice) != 0) {
      failure = __LINE__ * 100 + (int)i;
    }
    free(source);
    free(once);
    free(twice);
    if (failure != 0) {
      return failure;
    }
  }
  return 0;
}

/* What the file means, compared through everything the API says about it -- and, for calibrations,
 * the C they compile to, which walks every curve. */
static int same_definition(const FramedslDefinition *a, const FramedslDefinition *b) {
  if (framedsl_packet_count(a) != framedsl_packet_count(b) || framedsl_cal_count(a) != framedsl_cal_count(b)) {
    return __LINE__;
  }
  for (size_t p = 0; p < framedsl_packet_count(a); ++p) {
    if (strcmp(framedsl_packet_name(a, p), framedsl_packet_name(b, p)) != 0
        || framedsl_field_count(a, p) != framedsl_field_count(b, p)) {
      return __LINE__;
    }
    for (size_t f = 0; f < framedsl_field_count(a, p); ++f) {
      FramedslField x;
      FramedslField y;
      if (!framedsl_field(a, p, f, &x) || !framedsl_field(b, p, f, &y) || strcmp(x.name, y.name) != 0
          || x.variable != y.variable || x.width_bits != y.width_bits || x.max_bits != y.max_bits
          || x.offset_known != y.offset_known || x.offset_bits != y.offset_bits || x.has_fixed != y.has_fixed
          || x.fixed != y.fixed || x.bit_order != y.bit_order || x.byte_order != y.byte_order) {
        return __LINE__;
      }
    }
  }
  size_t length_a = framedsl_emit_c(a, NULL, NULL, 0);
  size_t length_b = framedsl_emit_c(b, NULL, NULL, 0);
  if (length_a != length_b) {
    return __LINE__;
  }
  char *c_a = malloc(length_a + 1);
  char *c_b = malloc(length_b + 1);
  int failure = 0;
  if (c_a == NULL || c_b == NULL) {
    failure = __LINE__;
  } else {
    framedsl_emit_c(a, NULL, c_a, length_a + 1);
    framedsl_emit_c(b, NULL, c_b, length_b + 1);
    failure = strcmp(c_a, c_b) == 0 ? 0 : __LINE__;
  }
  free(c_a);
  free(c_b);
  return failure;
}

static int us15_tc11_formatting_keeps_the_meaning(const TestArguments *args) {
  (void)args;
  for (size_t i = 0; i < sizeof(FILES) / sizeof(*FILES); ++i) {
    char *source = read_file(FILES[i]);
    char *formatted = NULL;
    if (source == NULL || framedsl_format(source, &formatted) != FRAMEDSL_OK) {
      free(source);
      return __LINE__ * 100 + (int)i;
    }
    /* Loaded as the same file, so imports resolve the same way. */
    FramedslResult *before = framedsl_load_source(source, FILES[i]);
    FramedslResult *after = framedsl_load_source(formatted, FILES[i]);
    int failure = 0;
    if (!framedsl_ok(before) || !framedsl_ok(after)) {
      failure = __LINE__;
    } else {
      failure = same_definition(framedsl_definition(before), framedsl_definition(after));
    }
    framedsl_result_free(before);
    framedsl_result_free(after);
    free(source);
    free(formatted);
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* ---- The command line ------------------------------------------------------------------------------- */

typedef struct {
  int status;
  char output[4096];
} Run;

static Run run_shell(const char *command) {
  Run run = { .status = -1, .output = { 0 } };
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return run;
  }
  size_t used = fread(run.output, 1, sizeof(run.output) - 1, pipe);
  run.output[used] = '\0';
  int closed = pclose(pipe);
  run.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
  return run;
}

static bool write_file(const TestArguments *args, const char *name, const char *content, char *path, size_t size) {
  snprintf(path, size, "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static bool file_is(const char *path, const char *content) {
  char *text = read_file(path);
  bool same = text != NULL && strcmp(text, content) == 0;
  free(text);
  return same;
}

static int us15_tc12_format_rewrites_in_place_and_check_writes_nothing(const TestArguments *args) {
  const char *messy = "pkt TM {\n\tid:8=1;\n};\n";
  const char *tidy = "pkt TM {\n  id: 8 = 1;\n};\n";
  char a[1024];
  char b[1024];
  if (!write_file(args, "a.fdsl", messy, a, sizeof(a)) || !write_file(args, "b.fdsl", tidy, b, sizeof(b))) {
    return __LINE__;
  }

  char command[3000];
  snprintf(command, sizeof(command), "%s %s %s --format --check 2>/dev/null", FRAMEDSL_BIN, a, b);
  Run run = run_shell(command);
  char expected[1100];
  snprintf(expected, sizeof(expected), "%s\n", a);
  if (run.status != 1 || strcmp(run.output, expected) != 0 || !file_is(a, messy)) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s %s --format 2>/dev/null", FRAMEDSL_BIN, a, b);
  run = run_shell(command);
  if (run.status != 0 || run.output[0] != '\0' || !file_is(a, tidy) || !file_is(b, tidy)) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s %s --format --check 2>/dev/null", FRAMEDSL_BIN, a, b);
  run = run_shell(command);
  return run.status == 0 && run.output[0] == '\0' ? 0 : __LINE__;
}

static int us15_tc13_standard_input_goes_to_standard_output(const TestArguments *args) {
  (void)args;
  char command[1024];
  snprintf(command, sizeof(command), "printf 'pkt TM {\\n\\tid:8=1;\\n};' | %s - --format 2>/dev/null", FRAMEDSL_BIN);
  Run run = run_shell(command);
  return run.status == 0 && strcmp(run.output, "pkt TM {\n  id: 8 = 1;\n};\n") == 0 ? 0 : __LINE__;
}

static int us15_tc14_a_broken_file_is_left_alone(const TestArguments *args) {
  const char *broken = "pkt TM {\n\tid: 8\n};\n";
  char path[1024];
  if (!write_file(args, "broken.fdsl", broken, path, sizeof(path))) {
    return __LINE__;
  }
  char command[2048];
  snprintf(command, sizeof(command), "%s %s --format 2>/dev/null", FRAMEDSL_BIN, path);
  Run run = run_shell(command);
  char expected[1200];
  snprintf(expected, sizeof(expected), "%s:3:1: error: expected ';', found '}'\n", path);
  return run.status == 1 && strcmp(run.output, expected) == 0 && file_is(path, broken) ? 0 : __LINE__;
}

void register_user_story15(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us15_tc01_indentation_and_spacing, "two spaces a level, and single spaces", false);
  ADD_TEST_CASE(us15_tc02_fields_align_within_a_run, "fields align within a run a nested block ends", false);
  ADD_TEST_CASE(us15_tc03_trailing_comments_align, "trailing comments align within a run", false);
  ADD_TEST_CASE(us15_tc04_attribute_groups, "one setting on a line, more one a line, aligned", false);
  ADD_TEST_CASE(us15_tc05_blank_lines, "blank lines: at most one, none inside braces, one between", false);
  ADD_TEST_CASE(
    us15_tc06_spelling_and_the_other_declarations, "numbers keep their spelling; imports, uses, curves", false
  );
  ADD_TEST_CASE(us15_tc07_every_comment_survives, "every comment survives, in order", false);
  ADD_TEST_CASE(us15_tc08_no_tab_survives, "no tab survives", false);
  ADD_TEST_CASE(us15_tc09_a_file_that_does_not_parse_is_refused, "a file that does not parse is refused", false);
  ADD_TEST_CASE(us15_tc10_formatting_twice_changes_nothing, "formatting twice changes nothing", false);
  ADD_TEST_CASE(us15_tc11_formatting_keeps_the_meaning, "a formatted file loads to the same definition", false);
  ADD_TEST_CASE(
    us15_tc12_format_rewrites_in_place_and_check_writes_nothing, "--format rewrites; --check only names", false
  );
  ADD_TEST_CASE(us15_tc13_standard_input_goes_to_standard_output, "'-' formats stdin to stdout", false);
  ADD_TEST_CASE(us15_tc14_a_broken_file_is_left_alone, "a broken file is left alone, its error printed", false);
}
