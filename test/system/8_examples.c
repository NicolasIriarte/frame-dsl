/* Story 8: the example a newcomer reads is not just illustration -- it runs, and this proves it.
 *
 * examples/uart_device.c is written against include/framedsl.h alone, the same discipline src/main.c
 * holds itself to, and is linked against build/libframedsl.a exactly as somebody else's program
 * would be. It is an example and not a test fixture; this story exists only so that a change which
 * breaks it fails here rather than failing the next person to read it.
 *
 * The cases check what the example claims -- a command is recognised from its bytes, the reply
 * carries what it should, the device remembers what it was told, and noise is reported rather than
 * fatal. The transmitted frames are checked as hex, because that is the part a newcomer compares
 * against the .fdsl by hand. */
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

#include "testsuite.h"

typedef struct {
  int status;
  char output[4096];
} Run;

static Run run_with(const char *arguments) {
  Run run = { .status = -1, .output = { 0 } };

  char command[1024];
  snprintf(command, sizeof(command), "%s %s 2>&1", UART_DEVICE_BIN, arguments);

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

static Run run_the_example(void) {
  char arguments[512];
  snprintf(arguments, sizeof(arguments), "%s/uart_device.fdsl", EXAMPLES_DIR);
  return run_with(arguments);
}

static int us8_tc01_a_command_is_recognised_from_its_bytes(const TestArguments *args) {
  (void)args;

  Run run = run_the_example();

  if (run.status != 0) {
    return __LINE__;
  }
  /* The driver was never told which packet to expect: this is framedsl_decode() naming it. */
  if (strstr(run.output, "received TC_SET_LIMIT (8 of 8 bytes)") == NULL) {
    return __LINE__;
  }
  if (strstr(run.output, "received TC_HK_REQ (3 of 3 bytes)") == NULL) {
    return __LINE__;
  }
  return 0;
}

static int us8_tc02_the_reply_carries_the_sequence_it_answers(const TestArguments *args) {
  (void)args;

  Run run = run_the_example();

  if (run.status != 0) {
    return __LINE__;
  }
  /* TM_ACK: id 0x81, sequence 1, result 0 -- the layout in examples/uart_device.fdsl, byte for
   * byte, which is the check a reader can make with the definition file open beside them. */
  if (strstr(run.output, "uart_transmit: 81 00 01 00") == NULL) {
    return __LINE__;
  }
  return 0;
}

static int us8_tc03_the_device_remembers_what_it_was_told(const TestArguments *args) {
  (void)args;

  Run run = run_the_example();

  if (run.status != 0) {
    return __LINE__;
  }
  /* TM_HK: id 0x82, sequence 2, channel 3, reading 0x11, limit 0x1068 -- 4200, set by the earlier
   * TC_SET_LIMIT. A device that forgot it would answer zero here and still look like it worked. */
  if (strstr(run.output, "uart_transmit: 82 00 02 03 00 00 00 11 00 00 10 68") == NULL) {
    return __LINE__;
  }
  return 0;
}

static int us8_tc04_noise_is_reported_not_fatal(const TestArguments *args) {
  (void)args;

  Run run = run_the_example();

  if (strstr(run.output, "bytes that are not a packet") == NULL) {
    return __LINE__;
  }
  /* Reported, and the two real frames ahead of it were still answered. */
  return run.status == 0 ? 0 : __LINE__;
}

static int us8_tc05_a_bad_path_fails_rather_than_crashing(const TestArguments *args) {
  (void)args;

  Run run = run_with("no/such/file.fdsl");
  return run.status == 1 ? 0 : __LINE__;
}

void register_user_story8(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us8_tc01_a_command_is_recognised_from_its_bytes,
    "the example's driver names the packets that arrive without being told",
    true
  );
  ADD_TEST_CASE(
    us8_tc02_the_reply_carries_the_sequence_it_answers,
    "the acknowledgement it sends back is the bytes it should be",
    false
  );
  ADD_TEST_CASE(
    us8_tc03_the_device_remembers_what_it_was_told, "housekeeping reports the limit an earlier telecommand set", false
  );
  ADD_TEST_CASE(us8_tc04_noise_is_reported_not_fatal, "bytes that are not a packet are reported, not fatal", false);
  ADD_TEST_CASE(
    us8_tc05_a_bad_path_fails_rather_than_crashing,
    "the example reports a missing definition rather than crashing",
    false
  );
}
