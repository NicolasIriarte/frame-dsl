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

/* Runs the binary with `arguments`, capturing stdout and stderr together. */
static Run run_with(const char *arguments) {
  Run run = { .status = -1, .output = { 0 } };

  char command[2048];
  snprintf(command, sizeof(command), "%s %s 2>&1", FRAMEDSL_BIN, arguments);

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

static Run run_on(const char *path) {
  return run_with(path);
}

/* Runs the binary against an example, decoding `hex`. The quoting is the tool's advertised shape:
 * one argument holding the whole capture, separators and all. */
static Run decode_example(const char *example, const char *flag, const char *hex) {
  char arguments[1024];
  snprintf(arguments, sizeof(arguments), "%s/%s %s \"%s\"", EXAMPLES_DIR, example, flag, hex);
  return run_with(arguments);
}

static int expect_parses(const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", EXAMPLES_DIR, name);
  Run run = run_on(path);
  return run.status == 0 ? 0 : __LINE__;
}

static int us1_tc01_parses_every_valid_example(const TestArguments *args) {
  (void)args;

  const char *names[] = { "tm_datagram.fdsl",     "ccsds_space_packet.fdsl", "packet_family.fdsl",
                          "bit_order.fdsl",       "calibrated_sensor.fdsl",  "composed_packets.fdsl",
                          "imported_device.fdsl", "common/link.fdsl",        "common/sensors.fdsl" };
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
    { "invalid/no_discriminator.fdsl",           11 },
    { "invalid/unbounded_valof.fdsl",            5  },
    { "invalid/ambiguous_packets.fdsl",          12 },
    { "invalid/forward_valof.fdsl",              7  },
    { "invalid/value_too_wide.fdsl",             8  },
    { "invalid/byte_order_unaligned.fdsl",       10 },
    { "invalid/odd_packet.fdsl",                 13 },
    { "invalid/odd_valof_width.fdsl",            13 },
    { "invalid/reservation_on_fixed_width.fdsl", 14 },
    { "invalid/repeat_varying_block.fdsl",       32 },
    { "invalid/repeat_with_override.fdsl",       33 },
    { "invalid/order_on_a_bit_string.fdsl",      15 },
    { "invalid/valof_on_a_bit_string.fdsl",      16 },
    { "invalid/duplicate_cal.fdsl",              17 },
    { "invalid/unknown_cal.fdsl",                18 },
    { "invalid/cal_unknown_name.fdsl",           19 },
    { "invalid/recursive_cal.fdsl",              20 },
    { "invalid/cal_on_a_bit_string.fdsl",        21 },
    { "invalid/cal_divides_by_zero.fdsl",        22 },
    { "invalid/unknown_block.fdsl",              23 },
    { "invalid/block_used_before_declared.fdsl", 24 },
    { "invalid/override_unknown_field.fdsl",     25 },
    { "invalid/import_missing.fdsl",             26 },
    { "invalid/import_cycle.fdsl",               27 },
    { "invalid/import_cycle_back.fdsl",          27 },
    { "invalid/import_clash.fdsl",               28 },
    { "invalid/pasted_field_clash.fdsl",         2  },
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
static int write_file(const TestArguments *args, const char *name, const char *content, char *path, size_t path_size) {
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

/* ---- Decoding a capture -------------------------------------------------------------------------
 *
 * Somebody has a frame as hex -- out of a log line, a tcpdump column, a colleague's message -- and
 * the definition that describes it. These cases are that, at the terminal. */

static int us1_tc08_decodes_a_frame_and_names_its_fields(const TestArguments *args) {
  (void)args;

  Run run = decode_example("packet_family.fdsl", "-d", "01 00 09");
  if (run.status != 0) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "TC_PING") == NULL) {
    FORCE_FAILURE();
  }
  /* The values, not merely the names: a tool that printed the field list without reading the bytes
   * would pass a test that only looked for "seq". */
  if (strstr(run.output, "seq") == NULL || strstr(run.output, "9") == NULL) {
    FORCE_FAILURE();
  }
  /* A packet it is not must not appear. */
  if (strstr(run.output, "TC_SET_MODE") != NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The hex arrives written however the thing it was copied from writes it. All of these are the same
 * three bytes, and all of them must decode to the same frame. */
static int us1_tc09_accepts_hex_however_it_is_written(const TestArguments *args) {
  (void)args;

  const char *forms[] = { "010009", "01 00 09", "01:00:09", "0x01,0x00,0x09", "01-00-09" };
  const char *flags[] = { "-d", "--decode" };

  for (size_t f = 0; f < sizeof(flags) / sizeof(*flags); ++f) {
    for (size_t i = 0; i < sizeof(forms) / sizeof(*forms); ++i) {
      Run run = decode_example("packet_family.fdsl", flags[f], forms[i]);
      if (run.status != 0 || strstr(run.output, "TC_PING") == NULL) {
        return __LINE__;
      }
    }
  }
  return 0;
}

/* Half a byte is a typo. Padding it would change the frame being asked about, and the answer would
 * be about a frame nobody has. */
static int us1_tc10_refuses_hex_that_is_not_hex(const TestArguments *args) {
  (void)args;

  const struct {
    const char *hex;
    const char *expected;
  } cases[] = {
    { "01 00 0",   "odd number of digits" },
    { "01 ZZ",     "not a hex digit"      },
    { "0 1 00 09", "one digit"            },
    { "",          "no bytes"             },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    Run run = decode_example("packet_family.fdsl", "-d", cases[i].hex);
    if (run.status == 0) {
      return __LINE__;
    }
    /* Which mistake it was, not merely that there was one: the message is the whole value of the
     * refusal, since the person reading it has to find the typo. */
    if (strstr(run.output, cases[i].expected) == NULL) {
      return __LINE__;
    }
  }
  return 0;
}

/* Frames back to back, which is what a capture looks like. TC_PAYLOAD's length is not knowable
 * without decoding it, so this is also the only thing that shows `consumed` doing its job. */
static int us1_tc11_walks_a_capture_of_several_frames(const TestArguments *args) {
  (void)args;

  Run run = decode_example("packet_family.fdsl", "-d", "01 00 01  03 00 02 00 02 AA BB  02 00 02 09");
  if (run.status != 0) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "TC_PING") == NULL || strstr(run.output, "TC_PAYLOAD") == NULL
      || strstr(run.output, "TC_SET_MODE") == NULL) {
    FORCE_FAILURE();
  }
  /* The payload's bytes, and its length as it arrived rather than the 256 it reserved. */
  if (strstr(run.output, "AA BB") == NULL || strstr(run.output, "[2 bytes]") == NULL) {
    FORCE_FAILURE();
  }
  if (strstr(run.output, "3 frames") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The three decode failures mean three different things to whoever is holding the capture, and the
 * terminal tells them apart as the API does. */
static int us1_tc12_tells_the_decode_failures_apart(const TestArguments *args) {
  (void)args;

  const struct {
    const char *hex;
    const char *expected;
  } cases[] = {
    { "FF 00 00 00",    "no packet"   },
    { "02 12",          "ran out"     },
    { "03 00 00 FF FF", "claims more" },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    Run run = decode_example("packet_family.fdsl", "-d", cases[i].hex);
    /* 1, not 2: the file was read and the bytes are the problem, which is the same distinction the
     * describe path already makes between a malformed file and a missing one. */
    if (run.status != 1) {
      return __LINE__;
    }
    if (strstr(run.output, cases[i].expected) == NULL) {
      return __LINE__;
    }
  }
  return 0;
}

static int us1_tc13_the_flag_needs_an_argument_and_the_old_shape_still_works(const TestArguments *args) {
  (void)args;

  char arguments[512];
  snprintf(arguments, sizeof(arguments), "%s/packet_family.fdsl -d", EXAMPLES_DIR);
  Run run = run_with(arguments);

  if (run.status != 2 || strstr(run.output, "--help") == NULL) {
    FORCE_FAILURE();
  }

  /* The flag before the file, which is how somebody who has used other tools will write it. */
  snprintf(arguments, sizeof(arguments), "-d \"010009\" %s/packet_family.fdsl", EXAMPLES_DIR);
  run = run_with(arguments);
  if (run.status != 0 || strstr(run.output, "TC_PING") == NULL) {
    FORCE_FAILURE();
  }

  /* And with no flag at all, the tool does what it has always done. */
  if (expect_parses("packet_family.fdsl") != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

/* ---- Encoding a frame ---------------------------------------------------------------------------
 *
 * The other direction: a packet name and some field values in, the frame's bytes out, to paste into a
 * simulator or push onto a bus by hand. */

static Run encode_example(const char *example, const char *arguments) {
  char line[1024];
  snprintf(line, sizeof(line), "%s/%s %s", EXAMPLES_DIR, example, arguments);
  return run_with(line);
}

static int us1_tc14_encodes_a_frame_that_decodes_back(const TestArguments *args) {
  (void)args;

  /* Both spellings, and fields left out encoding as zero: `seq` is never named here. -q, so that the
   * output is the frame and nothing else. */
  const char *flags[] = { "-e", "--encode" };
  for (size_t f = 0; f < sizeof(flags) / sizeof(*flags); ++f) {
    char arguments[128];
    snprintf(arguments, sizeof(arguments), "%s TC_SET_MODE mode=0x09 -q", flags[f]);
    Run run = encode_example("packet_family.fdsl", arguments);
    if (run.status != 0 || strcmp(run.output, "02 00 00 09\n") != 0) {
      return __LINE__;
    }
  }

  /* Without -q, the field left at zero is named -- and only that one: not `mode`, which was given,
   * and not `type`, which the definition fixes. */
  Run said = encode_example("packet_family.fdsl", "-e TC_SET_MODE mode=0x09");
  if (said.status != 0 || strstr(said.output, "TC_SET_MODE.seq not given") == NULL
      || strstr(said.output, ".mode not given") != NULL || strstr(said.output, ".type not given") != NULL) {
    return __LINE__;
  }

  /* A valof() packet, with its bit string written as hex. Only the hex reaches stdout, report and
   * all, so it can be piped: stderr is dropped here and stdout must be the frame and nothing else. */
  Run run = encode_example("packet_family.fdsl", "-e TC_PAYLOAD seq=2 len=2 body=AABB -r 2>/dev/null | cat");
  if (strcmp(run.output, "03 00 02 00 02 AA BB\n") != 0) {
    return __LINE__;
  }

  /* The report is of the frame read back, fixed values included. */
  run = encode_example("packet_family.fdsl", "-e TC_PAYLOAD seq=2 len=2 body=AABB --report");
  if (run.status != 0 || strstr(run.output, "pkt TC_PAYLOAD") == NULL
      || strstr(run.output, "[2 bytes] AA BB") == NULL) {
    return __LINE__;
  }

  /* And what it printed is what -d reads back: the round trip is the whole claim. */
  run = decode_example("packet_family.fdsl", "-d", "03 00 02 00 02 AA BB");
  if (run.status != 0 || strstr(run.output, "TC_PAYLOAD") == NULL || strstr(run.output, "AA BB") == NULL) {
    return __LINE__;
  }
  return 0;
}

/* Each refusal is a different mistake, and the message is what tells somebody which one they made. */
static int us1_tc15_refuses_what_the_packet_cannot_carry(const TestArguments *args) {
  (void)args;

  const struct {
    const char *arguments;
    int status;
    const char *expected;
  } cases[] = {
    { "-e TC_PING nope=1",       1, "has no field 'nope'"       },
    { "-e TC_PING seq=65536",    1, "too wide for its 16 bits"  },
    { "-e TC_NOPE seq=1",        1, "no packet named 'TC_NOPE'" },
    { "-e TC_PING type=7",       1, "fixed at 0x1"              },
    { "-e TC_PING seq=abc",      2, "not a number"              },
    { "-e TC_PAYLOAD body=AABB", 1, "set that field as well"    },
    { "-e TC_PAYLOAD len=2",     1, "must be zero"              },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    Run run = encode_example("packet_family.fdsl", cases[i].arguments);
    if (run.status != cases[i].status || strstr(run.output, cases[i].expected) == NULL) {
      return __LINE__;
    }
  }
  return 0;
}

static int us1_tc16_encode_arguments_that_would_need_a_guess_are_usage_errors(const TestArguments *args) {
  (void)args;

  const char *cases[] = {
    "-e TC_PING -d 010009", /* two questions at once */
    "-e", /* no packet */
    "seq=1", /* a value with nothing to put it in */
    "-q", /* nothing to be quiet about */
    "-e TC_PING seq=1 seq=2" /* which one? */
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    Run run = encode_example("packet_family.fdsl", cases[i]);
    if (run.status != 2) {
      return __LINE__;
    }
  }
  return 0;
}

void register_user_story1(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us1_tc01_parses_every_valid_example, "every example in examples/ parses", true);
  ADD_TEST_CASE(
    us1_tc02_rejects_each_invalid_example_for_its_documented_check,
    "each invalid example is rejected for the check it names",
    false
  );
  ADD_TEST_CASE(
    us1_tc03_reports_a_syntax_error_with_its_position, "a syntax error is reported with file, line and column", false
  );
  ADD_TEST_CASE(us1_tc04_reports_every_error_in_one_run, "several errors are reported in one run", false);
  ADD_TEST_CASE(
    us1_tc05_a_missing_file_is_distinguishable_from_a_bad_one,
    "a missing file exits differently from a malformed one",
    false
  );
  ADD_TEST_CASE(us1_tc06_reports_what_it_found, "a successful run names the packets it read", false);
  ADD_TEST_CASE(us1_tc07_no_arguments_is_a_usage_error, "running with no arguments explains itself", false);
  ADD_TEST_CASE(us1_tc08_decodes_a_frame_and_names_its_fields, "-d decodes a frame and prints its fields", false);
  ADD_TEST_CASE(
    us1_tc09_accepts_hex_however_it_is_written, "hex is accepted with any separators, under either flag", false
  );
  ADD_TEST_CASE(us1_tc10_refuses_hex_that_is_not_hex, "malformed hex is refused, naming the mistake", false);
  ADD_TEST_CASE(
    us1_tc11_walks_a_capture_of_several_frames, "a capture of several frames is walked frame by frame", false
  );
  ADD_TEST_CASE(
    us1_tc12_tells_the_decode_failures_apart,
    "unrecognised, truncated and over-bound are told apart at the terminal",
    false
  );
  ADD_TEST_CASE(
    us1_tc13_the_flag_needs_an_argument_and_the_old_shape_still_works,
    "-d needs hex, takes either position, and describing still works",
    false
  );
  ADD_TEST_CASE(us1_tc14_encodes_a_frame_that_decodes_back, "-e prints a frame as hex that -d reads back", false);
  ADD_TEST_CASE(
    us1_tc15_refuses_what_the_packet_cannot_carry, "-e refuses each kind of bad field with its own message", false
  );
  ADD_TEST_CASE(
    us1_tc16_encode_arguments_that_would_need_a_guess_are_usage_errors,
    "-e with -d, without a packet, or with a field twice is a usage error",
    false
  );
}
