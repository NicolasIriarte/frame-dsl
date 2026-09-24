/* Story 19: `--list-packets` and `--list-fields <packet>` (#85) -- names for a program to read.
 *
 * What the Bash and zsh completions ask the tool, so the answer has to be exactly the names, one per
 * line, and nothing at all -- on either stream -- when there is no answer: a completion that printed a
 * diagnostic would print it into the middle of somebody's command line. The completions themselves
 * are tested by completions/test_completion.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.h"

typedef struct {
  int status;
  char out[4096];
  char err[4096];
} Run;

static void slurp(const char *path, char *buffer, size_t capacity) {
  buffer[0] = '\0';
  FILE *handle = fopen(path, "r");
  if (handle == NULL) {
    return;
  }
  size_t got = fread(buffer, 1, capacity - 1, handle);
  buffer[got] = '\0';
  fclose(handle);
}

/* Runs `command` through the shell with FRAMEDSL_BIN in front, stdout and stderr kept apart. */
static Run run_with(const char *arguments) {
  Run run = { .status = -1 };
  char out_path[] = "/tmp/frame-dsl-list-out-XXXXXX";
  char err_path[] = "/tmp/frame-dsl-list-err-XXXXXX";
  int out_fd = mkstemp(out_path);
  int err_fd = mkstemp(err_path);
  if (out_fd >= 0) {
    close(out_fd);
  }
  if (err_fd >= 0) {
    close(err_fd);
  }
  if (out_fd < 0 || err_fd < 0) {
    return run;
  }

  char command[2048];
  snprintf(command, sizeof(command), "%s %s >%s 2>%s", FRAMEDSL_BIN, arguments, out_path, err_path);
  int status = system(command);
  run.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  slurp(out_path, run.out, sizeof(run.out));
  slurp(err_path, run.err, sizeof(run.err));
  remove(out_path);
  remove(err_path);
  return run;
}

static bool answers(const Run *run, const char *out) {
  return run->status == 0 && run->err[0] == '\0' && strcmp(run->out, out) == 0;
}

static bool silent_failure(const Run *run) {
  return run->status == 1 && run->out[0] == '\0' && run->err[0] == '\0';
}

static int us19_tc01_packets_are_listed_one_per_line(const TestArguments *args) {
  (void)args;
  Run run = run_with(EXAMPLES_DIR "/uart_device.fdsl --list-packets");
  return answers(&run, "TC_HK_REQ\nTC_SET_LIMIT\nTC_WRITE\nTM_ACK\nTM_HK\n") ? 0 : __LINE__;
}

/* The fields -e takes: `id` is fixed by the definition, and -e refuses it, so it is not offered. */
static int us19_tc02_fields_are_the_ones_encode_takes(const TestArguments *args) {
  (void)args;
  Run run = run_with(EXAMPLES_DIR "/uart_device.fdsl --list-fields TC_WRITE");
  if (!answers(&run, "size\ndata\ncrc\n")) {
    return __LINE__;
  }
  /* An instance's fields by the name -e takes them. */
  run = run_with(EXAMPLES_DIR "/composed_packets.fdsl --list-fields TC_SET_LIMIT");
  return run.status == 0 && strstr(run.out, "hdr.") != NULL ? 0 : __LINE__;
}

static int us19_tc03_no_answer_is_silent(const TestArguments *args) {
  (void)args;
  Run run = run_with(EXAMPLES_DIR "/uart_device.fdsl --list-fields NO_SUCH_PACKET");
  if (!silent_failure(&run)) {
    return __LINE__;
  }
  run = run_with(EXAMPLES_DIR "/invalid/unknown_block.fdsl --list-packets");
  if (!silent_failure(&run)) {
    return __LINE__;
  }
  run = run_with(EXAMPLES_DIR "/no_such_file.fdsl --list-packets");
  return silent_failure(&run) ? 0 : __LINE__;
}

/* A buffer not yet saved, as an editor's completion would have it. */
static int us19_tc04_standard_input_is_listed_too(const TestArguments *args) {
  (void)args;
  Run run = run_with("- --list-packets < " EXAMPLES_DIR "/uart_device.fdsl");
  return answers(&run, "TC_HK_REQ\nTC_SET_LIMIT\nTC_WRITE\nTM_ACK\nTM_HK\n") ? 0 : __LINE__;
}

static int us19_tc05_listing_does_nothing_else(const TestArguments *args) {
  (void)args;
  const char *conflicts[] = {
    EXAMPLES_DIR "/uart_device.fdsl --list-packets -e TC_WRITE",
    EXAMPLES_DIR "/uart_device.fdsl --list-packets --check",
    EXAMPLES_DIR "/uart_device.fdsl --list-packets --list-fields TC_WRITE",
    EXAMPLES_DIR "/uart_device.fdsl " EXAMPLES_DIR "/tm_datagram.fdsl --list-packets",
  };
  for (size_t i = 0; i < sizeof(conflicts) / sizeof(*conflicts); ++i) {
    Run run = run_with(conflicts[i]);
    if (run.status != 2 || strstr(run.err, "--help") == NULL) {
      return __LINE__;
    }
  }
  return 0;
}

void register_user_story19(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us19_tc01_packets_are_listed_one_per_line, "--list-packets prints the names, one per line", false);
  ADD_TEST_CASE(us19_tc02_fields_are_the_ones_encode_takes, "--list-fields prints the fields -e takes", false);
  ADD_TEST_CASE(us19_tc03_no_answer_is_silent, "no answer is exit 1 and nothing on either stream", false);
  ADD_TEST_CASE(us19_tc04_standard_input_is_listed_too, "a definition on stdin is listed too", false);
  ADD_TEST_CASE(us19_tc05_listing_does_nothing_else, "listing with another mode is a usage error", false);
}
