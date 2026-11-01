/* Story 13: checking a file the way an editor does (#70).
 *
 * An editor does not read the tool's human output; it runs `--check`, often on a buffer piped through
 * standard input, and parses one diagnostic per line. So these cases assert the exact bytes of that
 * output and the exit status, and -- through the public API -- the span an editor underlines. */
/* realpath() is XSI, which the _POSIX_C_SOURCE this story is built with does not reach everywhere. */
#define _XOPEN_SOURCE 700

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

typedef struct {
  int status;
  char output[8192];
} Run;

/* Runs `command` through the shell, capturing stdout alone: that is what an editor reads. */
static Run run_shell(const char *command) {
  Run run = { .status = -1, .output = { 0 } };

  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return run;
  }
  size_t used = 0;
  size_t got;
  while ((got = fread(run.output + used, 1, sizeof(run.output) - used - 1, pipe)) > 0) {
    used += got;
    if (used + 1 >= sizeof(run.output)) {
      break;
    }
  }
  run.output[used] = '\0';

  int closed = pclose(pipe);
  run.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
  return run;
}

static Run run_with(const char *arguments) {
  char command[4096];
  snprintf(command, sizeof(command), "%s %s 2>/dev/null", FRAMEDSL_BIN, arguments);
  return run_shell(command);
}

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

static const char BROKEN[] = "pkt TM {\n  id: 8 := 1;\n  len: 8\n  data: valof(len);\n};\n";
static const char CLEAN[] = "pkt TM {\n  id: 8 := 1;\n  len: 8;\n};\n";

static int us13_tc01_an_error_is_one_plain_line(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "broken.fdsl", BROKEN, path, sizeof(path))) {
    return __LINE__;
  }
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "%s --check", path);
  Run run = run_with(arguments);

  /* The fix, where there is one, follows as a note at the place it would edit (#73). */
  char expected[2200];
  snprintf(
    expected,
    sizeof(expected),
    "%s:4:3: error: expected ';', found the name 'data'\n%s:3:9: note: insert ';'\n",
    path,
    path
  );
  if (run.status != 1) {
    return __LINE__;
  }
  return strcmp(run.output, expected) == 0 ? 0 : __LINE__;
}

/* The check number goes at the end, where a reader skimming the message does not trip on it. */
static int us13_tc02_a_check_number_follows_the_message(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "wide.fdsl", "pkt TM {\n  id: 4 := 0x1F;\n  rest: 4;\n};\n", path, sizeof(path))) {
    return __LINE__;
  }
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "%s --check", path);
  Run run = run_with(arguments);

  char prefix[1200];
  snprintf(prefix, sizeof(prefix), "%s:2:", path);
  if (run.status != 1 || strncmp(run.output, prefix, strlen(prefix)) != 0) {
    return __LINE__;
  }
  if (strstr(run.output, ": error: ") == NULL || strstr(run.output, " [check 8]\n") == NULL) {
    return __LINE__;
  }
  /* No colour, and nothing of the logger's. */
  return strchr(run.output, '\033') == NULL && strstr(run.output, "[FAIL]") == NULL ? 0 : __LINE__;
}

static int us13_tc03_a_clean_file_prints_nothing(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "clean.fdsl", CLEAN, path, sizeof(path))) {
    return __LINE__;
  }
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "%s --check", path);
  Run run = run_with(arguments);
  return run.status == 0 && run.output[0] == '\0' ? 0 : __LINE__;
}

static int us13_tc04_several_files_are_each_checked(const TestArguments *args) {
  char broken[1024];
  char clean[1024];
  char other[1024];
  if (!write_file(args, "a.fdsl", BROKEN, broken, sizeof(broken))
      || !write_file(args, "b.fdsl", CLEAN, clean, sizeof(clean))
      || !write_file(args, "c.fdsl", BROKEN, other, sizeof(other))) {
    return __LINE__;
  }
  char arguments[3400];
  snprintf(arguments, sizeof(arguments), "%s %s %s --check", broken, clean, other);
  Run run = run_with(arguments);

  char expected[5000];
  snprintf(
    expected,
    sizeof(expected),
    "%s:4:3: error: expected ';', found the name 'data'\n%s:3:9: note: insert ';'\n"
    "%s:4:3: error: expected ';', found the name 'data'\n%s:3:9: note: insert ';'\n",
    broken,
    broken,
    other,
    other
  );
  return run.status == 1 && strcmp(run.output, expected) == 0 ? 0 : __LINE__;
}

/* A mistake in an imported file is reported where it is, so an editor can open that file at it. */
static int us13_tc05_an_imported_problem_names_its_own_file(const TestArguments *args) {
  char library[1024];
  char top[1024];
  if (!write_file(args, "lib/link.fdsl", "block Header {\n  id: 8 := 1\n};\n", library, sizeof(library))
      || !write_file(args, "top.fdsl", "import \"lib/link.fdsl\";\npkt TM {\n  h: Header;\n};\n", top, sizeof(top))) {
    return __LINE__;
  }
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "%s --check", top);
  Run run = run_with(arguments);

  char expected[1200];
  snprintf(expected, sizeof(expected), "%s:3:1: error: ", library);
  return run.status == 1 && strncmp(run.output, expected, strlen(expected)) == 0 ? 0 : __LINE__;
}

/* The buffer as typed, checked where it will be saved: imports resolve from --stdin-name's
 * directory, not from wherever the editor happens to be running the tool. */
static int us13_tc06_standard_input_is_checked_as_the_named_file(const TestArguments *args) {
  char library[1024];
  char top[1024];
  if (!write_file(args, "lib/link.fdsl", "block Header {\n  id: 8 := 1;\n};\n", library, sizeof(library))
      || !write_file(args, "top.fdsl", "# not what is checked\n", top, sizeof(top))) {
    return __LINE__;
  }

  char command[PATH_MAX + 2048];
  snprintf(
    command,
    sizeof(command),
    "printf 'import \"lib/link.fdsl\";\\npkt TM {\\n  h: Header;\\n  len: 8\\n};\\n' | %s - --check --stdin-name %s",
    FRAMEDSL_BIN,
    top
  );
  Run run = run_shell(command);
  char expected[2400];
  snprintf(expected, sizeof(expected), "%s:5:1: error: expected ';', found '}'\n%s:4:9: note: insert ';'\n", top, top);
  if (run.status != 1 || strcmp(run.output, expected) != 0) {
    return __LINE__;
  }

  /* Fixed, it is clean -- which it could only be if the import resolved beside `top`. */
  snprintf(
    command,
    sizeof(command),
    "printf 'import \"lib/link.fdsl\";\\npkt TM {\\n  h: Header;\\n  len: 8;\\n};\\n' | %s - --check --stdin-name %s",
    FRAMEDSL_BIN,
    top
  );
  run = run_shell(command);
  if (run.status != 0 || run.output[0] != '\0') {
    return __LINE__;
  }

  /* Without a name, the same text is "<stdin>" and imports resolve from the working directory -- here
   * lib/, where there is no lib/link.fdsl. The binary's path is relative, so it is resolved first. */
  char binary[PATH_MAX];
  if (realpath(FRAMEDSL_BIN, binary) == NULL) {
    return __LINE__;
  }
  snprintf(
    command, sizeof(command), "cd %s/lib && printf 'import \"lib/link.fdsl\";\\n' | %s - --check", args->tmp_dir, binary
  );
  run = run_shell(command);
  return run.status == 1 && strncmp(run.output, "<stdin>:1:1: error: cannot read", 31) == 0 ? 0 : __LINE__;
}

static int us13_tc07_check_does_not_combine_with_the_other_modes(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "clean.fdsl", CLEAN, path, sizeof(path))) {
    return __LINE__;
  }
  const char *combinations[] = {
    "%s --check -d 01", "%s --check --list-packets",      "%s --check --emit-c", "%s --werror", "%s %s",
    "- - --check",      "%s --stdin-name x.fdsl --check",
  };
  for (size_t i = 0; i < sizeof(combinations) / sizeof(*combinations); ++i) {
    char arguments[2600];
    snprintf(arguments, sizeof(arguments), combinations[i], path, path);
    Run run = run_with(arguments);
    if (run.status != 2) {
      return __LINE__ * 100 + (int)i;
    }
  }
  return 0;
}

static int us13_tc08_an_unreadable_file_is_status_2(const TestArguments *args) {
  char arguments[1200];
  snprintf(arguments, sizeof(arguments), "%s/missing.fdsl --check", args->tmp_dir);
  Run run = run_with(arguments);
  char expected[1200];
  snprintf(expected, sizeof(expected), "%s/missing.fdsl: error: cannot read", args->tmp_dir);
  return run.status == 2 && strncmp(run.output, expected, strlen(expected)) == 0 ? 0 : __LINE__;
}

/* The span is what gets underlined: the whole offending name, not its first character. */
static int us13_tc09_a_diagnostic_spans_its_token(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string("pkt TM {\n  id: 8 := 1;\n  h: Heder;\n};\n", "t.fdsl");
  int failure = 0;
  if (framedsl_ok(result) || framedsl_diagnostic_count(result) != 1) {
    failure = __LINE__;
  } else {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, 0);
    if (diag->severity != FRAMEDSL_SEVERITY_ERROR || diag->check != 23) {
      failure = __LINE__;
    } else if (diag->line != 3 || diag->column != 3 || diag->end_line != 3 || diag->end_column != 4) {
      /* check 23 is reported at the instance's name, `h` -- one character, so one column wide */
      failure = __LINE__;
    }
  }
  framedsl_result_free(result);
  if (failure != 0) {
    return failure;
  }

  result = framedsl_load_string("pkt TM {\n  id: 8 := 1;\n  length 8;\n};\n", "t.fdsl");
  if (framedsl_ok(result) || framedsl_diagnostic_count(result) < 1) {
    failure = __LINE__;
  } else {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, 0);
    /* "expected ':', found the number 8" -- at the 8, and covering it */
    if (diag->line != 3 || diag->column != 10 || diag->end_line != 3 || diag->end_column != 11) {
      failure = __LINE__;
    }
  }
  framedsl_result_free(result);
  return failure;
}

/* Over every rejected example, which between them reach every pass that reports: nothing ends before
 * it starts, and anything with a position covers at least the start. */
static int us13_tc10_every_span_is_well_formed(const TestArguments *args) {
  (void)args;
  const char *names[] = {
    "no_discriminator.fdsl",
    "unbounded_valof.fdsl",
    "ambiguous_packets.fdsl",
    "forward_valof.fdsl",
    "value_too_wide.fdsl",
    "byte_order_unaligned.fdsl",
    "odd_packet.fdsl",
    "odd_valof_width.fdsl",
    "reservation_on_fixed_width.fdsl",
    "order_on_a_bit_string.fdsl",
    "valof_on_a_bit_string.fdsl",
    "duplicate_cal.fdsl",
    "unknown_cal.fdsl",
    "cal_unknown_name.fdsl",
    "recursive_cal.fdsl",
    "cal_on_a_bit_string.fdsl",
    "cal_divides_by_zero.fdsl",
    "unknown_block.fdsl",
    "block_used_before_declared.fdsl",
    "override_unknown_field.fdsl",
    "import_missing.fdsl",
    "import_cycle.fdsl",
    "import_clash.fdsl",
    "enum_width_out_of_range.fdsl",
    "enum_duplicate_name.fdsl",
    "enum_values_out_of_order.fdsl",
    "enum_value_too_wide.fdsl",
    "enum_unknown_name.fdsl",
    "enum_with_cal.fdsl",
    "enum_pasted.fdsl",
    "if_reads_absent_field.fdsl",
    "if_part_not_whole_bytes.fdsl",
    "if_number_on_enum.fdsl",
    "if_value_too_wide.fdsl",
    "discriminator_after_condition.fdsl",
    "if_in_repeated_block.fdsl",
    "if_reads_later_field.fdsl",
    "if_ordering_on_enum.fdsl",
    "if_mask_too_wide.fdsl",
    "if_mask_clears_value.fdsl",
  };
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
    char path[512];
    snprintf(path, sizeof(path), "%s/invalid/%s", EXAMPLES_DIR, names[i]);
    FramedslResult *result = framedsl_load_file(path);
    int failure = framedsl_diagnostic_count(result) == 0 ? __LINE__ : 0;
    for (size_t d = 0; d < framedsl_diagnostic_count(result) && failure == 0; ++d) {
      const FramedslDiagnostic *diag = framedsl_diagnostic(result, d);
      if (diag->line == 0) {
        continue;
      }
      bool ordered = diag->end_line > diag->line || (diag->end_line == diag->line && diag->end_column >= diag->column);
      if (!ordered) {
        failure = __LINE__;
      }
    }
    framedsl_result_free(result);
    if (failure != 0) {
      return failure * 100 + (int)i;
    }
  }
  return 0;
}

/* framedsl_load_source() is framedsl_load_file() with the text supplied: same name, same imports. */
static int us13_tc11_load_source_behaves_as_the_named_file(const TestArguments *args) {
  char library[1024];
  char top[1024];
  if (!write_file(args, "lib/link.fdsl", "block Header {\n  id: 8 := 1;\n};\n", library, sizeof(library))) {
    return __LINE__;
  }
  snprintf(top, sizeof(top), "%s/not_saved_yet.fdsl", args->tmp_dir);

  FramedslResult *result = framedsl_load_source("import \"lib/link.fdsl\";\npkt TM {\n  h: Header;\n};\n", top);
  int failure = 0;
  if (!framedsl_ok(result) || strcmp(framedsl_source_name(result), top) != 0) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  return failure;
}

/* Every path an editor names must still come out as the tool on somebody's PATH. */
static int us13_tc12_make_install_puts_the_tool_on_a_path(const TestArguments *args) {
  char command[2048];
  snprintf(
    command,
    sizeof(command),
    "make -s install PREFIX=%s/prefix >/dev/null 2>&1 && %s/prefix/bin/frame-dsl %s/tm_datagram.fdsl --check",
    args->tmp_dir,
    args->tmp_dir,
    EXAMPLES_DIR
  );
  Run run = run_shell(command);
  return run.status == 0 && run.output[0] == '\0' ? 0 : __LINE__;
}

void register_user_story13(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us13_tc01_an_error_is_one_plain_line, "--check prints an error as one gcc-style line", false);
  ADD_TEST_CASE(
    us13_tc02_a_check_number_follows_the_message, "the check number follows the message, uncoloured", false
  );
  ADD_TEST_CASE(us13_tc03_a_clean_file_prints_nothing, "a clean file prints nothing and exits 0", false);
  ADD_TEST_CASE(us13_tc04_several_files_are_each_checked, "several files are each checked and attributed", false);
  ADD_TEST_CASE(us13_tc05_an_imported_problem_names_its_own_file, "a problem in an import names that file", false);
  ADD_TEST_CASE(
    us13_tc06_standard_input_is_checked_as_the_named_file, "stdin is checked as the file --stdin-name names", false
  );
  ADD_TEST_CASE(us13_tc07_check_does_not_combine_with_the_other_modes, "--check refuses to be combined", false);
  ADD_TEST_CASE(us13_tc08_an_unreadable_file_is_status_2, "an unreadable file is reported with status 2", false);
  ADD_TEST_CASE(us13_tc09_a_diagnostic_spans_its_token, "a diagnostic spans the token it is about", false);
  ADD_TEST_CASE(us13_tc10_every_span_is_well_formed, "every span over the invalid examples is well formed", false);
  ADD_TEST_CASE(
    us13_tc11_load_source_behaves_as_the_named_file, "framedsl_load_source() loads as the named file", false
  );
  ADD_TEST_CASE(
    us13_tc12_make_install_puts_the_tool_on_a_path, "make install puts a working frame-dsl on a path", false
  );
}
