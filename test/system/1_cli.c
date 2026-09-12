/* Story 1: using frame-dsl the way someone at a terminal does.
 *
 * This is a system test, so it reaches for nothing inside the project -- it runs the real binary
 * against the real files in examples/ and reads what comes back. It is compiled without src/ on its
 * include path, so it could not include an internal header even by accident. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "testsuite.h"

typedef struct {
  int status;
  char output[8192];
} Run;

/* Runs the binary on `path`, capturing stdout and stderr together. */
static Run run_on(const char *path) {
  Run run = {.status = -1, .output = {0}};

  char command[1024];
  snprintf(command, sizeof(command), "%s %s 2>&1", FRAMEDSL_BIN, path);

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

static int expect_parses(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, name);
  Run run = run_on(path);
  return run.status == 0 ? 0 : __LINE__;
}

static int us1_tc01_parses_every_valid_example(const TestArguments *args) {
  (void)args;

  const char *names[] = {"tm_datagram.fdsl", "ccsds_space_packet.fdsl", "packet_family.fdsl",
                         "bit_order.fdsl"};
  for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i) {
    int failure = expect_parses(names[i]);
    if (failure != 0) {
      return failure;
    }
  }
  return 0;
}

/* Each file under examples/invalid/ names, in its own header comment, the one check it exists to
 * trip. Asserting the number rather than merely "it failed" is the point: a file rejected for the
 * wrong reason is a green test hiding a broken checker.
 *
 * This is also the documentation staying honest. If someone changes a check so a fixture starts
 * failing for a different reason, the fixture's comment has become a lie and this says so. */
static int us1_tc02_rejects_each_invalid_example_for_its_documented_check(const TestArguments *args) {
  (void)args;

  const struct {
    const char *name;
    int check;
  } cases[] = {
      {"invalid/no_discriminator.fdsl", 11},   {"invalid/unbounded_valof.fdsl", 5},
      {"invalid/ambiguous_packets.fdsl", 12},  {"invalid/forward_valof.fdsl", 7},
      {"invalid/value_too_wide.fdsl", 8},      {"invalid/byte_order_unaligned.fdsl", 10},
      {"invalid/odd_packet.fdsl", 13},         {"invalid/odd_valof_width.fdsl", 13},
      {"invalid/reservation_on_fixed_width.fdsl", 14},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, cases[i].name);
    Run run = run_on(path);

    if (run.status != 1) {
      return __LINE__;
    }

    char wanted[32];
    snprintf(wanted, sizeof(wanted), "[check %d]", cases[i].check);
    if (strstr(run.output, wanted) == NULL) {
      return __LINE__;
    }
  }
  return 0;
}

/* Each case gets a private, already-empty directory to write to. */
static int write_file(const TestArguments *args, const char *name, const char *content, char *path,
                      size_t path_size) {
  snprintf(path, path_size, "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return -1;
  }
  fputs(content, handle);
  fclose(handle);
  return 0;
}

static int us1_tc03_reports_a_syntax_error_with_its_position(const TestArguments *args) {
  char path[1024];
  if (write_file(args, "broken.fdsl", "pkt TM {\n  hdr 8;\n};\n", path, sizeof(path)) != 0) {
    FORCE_FAILURE();
  }

  Run run = run_on(path);
  if (run.status != 1) {
    FORCE_FAILURE();
  }
  /* The position matters more than the wording: it is what someone uses to find the mistake. */
  if (strstr(run.output, ":2:7:") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us1_tc04_reports_every_error_in_one_run(const TestArguments *args) {
  char path[1024];
  if (write_file(args, "messy.fdsl", "pkt TM {\n  a 8;\n  b: ;\n  c: 8\n};\n", path, sizeof(path)) != 0) {
    FORCE_FAILURE();
  }

  Run run = run_on(path);
  if (run.status != 1) {
    FORCE_FAILURE();
  }

  size_t reported = 0;
  for (const char *at = run.output; (at = strstr(at, "[FAIL]")) != NULL; at += 6) {
    reported += 1;
  }
  if (reported < 3) {
    FORCE_FAILURE();
  }
  return 0;
}

/* A missing file is not a malformed file, and the exit status says which. */
static int us1_tc05_a_missing_file_is_distinguishable_from_a_bad_one(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/not_here.fdsl", args->tmp_dir);

  Run run = run_on(path);
  if (run.status != 2) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "cannot read") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us1_tc06_reports_what_it_found(const TestArguments *args) {
  (void)args;

  char path[512];
  snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, "packet_family.fdsl");
  Run run = run_on(path);

  if (run.status != 0) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "TC_PING") == NULL || strstr(run.output, "TC_PAYLOAD") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us1_tc07_no_arguments_is_a_usage_error(const TestArguments *args) {
  (void)args;

  Run run = run_on("");
  if (run.status != 2) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "usage") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

void register_user_story1(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us1_tc01_parses_every_valid_example, "every example in examples/ parses", true);
  ADD_TEST_CASE(us1_tc02_rejects_each_invalid_example_for_its_documented_check,
                "each invalid example is rejected for the check it names", false);
  ADD_TEST_CASE(us1_tc03_reports_a_syntax_error_with_its_position,
                "a syntax error is reported with file, line and column", false);
  ADD_TEST_CASE(us1_tc04_reports_every_error_in_one_run, "several errors are reported in one run", false);
  ADD_TEST_CASE(us1_tc05_a_missing_file_is_distinguishable_from_a_bad_one,
                "a missing file exits differently from a malformed one", false);
  ADD_TEST_CASE(us1_tc06_reports_what_it_found, "a successful run names the packets it read", false);
  ADD_TEST_CASE(us1_tc07_no_arguments_is_a_usage_error, "running with no arguments explains itself", false);
}
