/* Story 30: the value column (#144). `:=` and `?=` start in one column among the lines of a run that
 * have a value, past the widest width among them, so that a definition's values read as a list. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* A value starts past the widest width among the lines that have one, and a line without one neither
 * sets that column nor follows it (#144). */
static int us30_tc01_values_share_a_column(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n  id: 8 := 0x40;\n  spare: 8 ?= 0;\n  power: Health ?= OK;\n  channels: Channel[4];\n};\n",
    "pkt TM {\n"
    "  id:       8      := 0x40;\n"
    "  spare:    8      ?= 0;\n"
    "  power:    Health ?= OK;\n"
    "  channels: Channel[4];\n"
    "};\n"
  );
}

/* A use's overrides are part of its line, and take no part in the column. */
static int us30_tc02_overrides_stay_on_their_line(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n  hdr: Header {id:=1;seq?=2;};\n  a: 8 := 1;\n  bb: 16 ?= 2;\n};\n",
    "pkt TM {\n"
    "  hdr: Header { id := 1; seq ?= 2; };\n"
    "  a:   8  := 1;\n"
    "  bb:  16 ?= 2;\n"
    "};\n"
  );
}

/* A sign stays against its number, on a field and in an override (#10). */
static int us30_tc06_a_negative_value_keeps_its_sign(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n  hdr: Header {a:=- 1;};\n  t: i16:=-160;\n  gain: f32 ?= -1.5e3;\n};\n",
    "pkt TM {\n"
    "  hdr:  Header { a := -1; };\n"
    "  t:    i16 := -160;\n"
    "  gain: f32 ?= -1.5e3;\n"
    "};\n"
  );
}

static int us30_tc03_a_comment_after_a_value_aligns(const TestArguments *args) {
  (void)args;
  return expect_format(
    "pkt TM {\n  id: 8 := 1; # the id\n  mode: 16 ?= 2;   # the mode\n  flags: 8;  # flags\n};\n",
    "pkt TM {\n"
    "  id:    8  := 1;  # the id\n"
    "  mode:  16 ?= 2;  # the mode\n"
    "  flags: 8;        # flags\n"
    "};\n"
  );
}

static int us30_tc04_the_column_is_stable_and_checked(const TestArguments *args) {
  const char *loose = "pkt TM {\n  id: 8 := 1;\n  mode: 16 ?= 2;\n};\n";
  const char *tidy = "pkt TM {\n  id:   8  := 1;\n  mode: 16 ?= 2;\n};\n";
  int failure = expect_format(tidy, tidy);
  char path[1024];
  snprintf(path, sizeof(path), "%s/column.fdsl", args->tmp_dir);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return __LINE__;
  }
  fputs(loose, handle);
  fclose(handle);
  if (failure == 0) {
    char command[1200];
    snprintf(command, sizeof(command), "%s %s --format --check 2>/dev/null", FRAMEDSL_BIN, path);
    Run run = run_shell(command);
    char expected[1100];
    snprintf(expected, sizeof(expected), "%s\n", path);
    failure = run.status == 1 && strcmp(run.output, expected) == 0 ? 0 : __LINE__;
  }
  return failure;
}

/* Every example is in the layout, the same as `make format-check` asks of it. */
static int us30_tc05_every_example_is_in_the_layout(const TestArguments *args) {
  (void)args;
  char command[1200];
  snprintf(
    command, sizeof(command), "%s examples/*.fdsl examples/invalid/*.fdsl --format --check 2>/dev/null", FRAMEDSL_BIN
  );
  Run run = run_shell(command);
  return run.output[0] == '\0' ? 0 : __LINE__;
}

void register_user_story30(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us30_tc01_values_share_a_column, "values share a column among the lines that have one", false);
  ADD_TEST_CASE(us30_tc02_overrides_stay_on_their_line, "a use's overrides stay on its line", false);
  ADD_TEST_CASE(us30_tc03_a_comment_after_a_value_aligns, "a comment after a value still aligns", false);
  ADD_TEST_CASE(
    us30_tc04_the_column_is_stable_and_checked, "the column is idempotent, and --check names a file", false
  );
  ADD_TEST_CASE(us30_tc05_every_example_is_in_the_layout, "every example is in the layout", false);
  ADD_TEST_CASE(us30_tc06_a_negative_value_keeps_its_sign, "a sign stays against its number", false);
}
