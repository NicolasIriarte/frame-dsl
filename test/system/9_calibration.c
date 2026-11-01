/* Story 9: calibrations, as somebody using the library sees them.
 *
 * A calibrated field reads and writes in engineering units through its curve, and in raw counts
 * exactly as before. A curve can be called by name without a field, and written out as C that
 * computes the same thing. Expected raw values are worked out by hand from the definition below, not
 * taken from the library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "allocations.h"
#include "framedsl.h"
#include "testsuite.h"

/* celsius and volts are lines; ohms is not, and declares its inverse; square is not, and does not. */
static const char *const SOURCE =
  "cal volts(raw)   = raw * 3.3 / 4095;\n"
  "cal current(raw) = volts(raw) / 0.05;\n"
  "cal celsius(raw) = raw * 0.0625 - 40;\n"
  "cal ohms(raw)    = 10000 * raw / (4095 - raw)\n"
  "                   inverse (eng) = 4095 * eng / (10000 + eng);\n"
  "cal square(raw)  = raw * raw;\n"
  "pkt TM {\n"
  "  id: 8 := 0x10;\n"
  "  [ @cal: celsius; ] board: 16;\n"
  "  [ @cal: volts; ]   bus: 12;\n"
  "  [ @cal: ohms; ]    sensor: 12;\n"
  "  [ @cal: square; ]  area: 8;\n"
  "  plain: 8;\n"
  "};\n";

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(Fixture *fixture) {
  fixture->result = framedsl_load_string(SOURCE, "calibration.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = framedsl_frame_new(fixture->definition);
  return framedsl_ok(fixture->result) && fixture->frame != NULL;
}

static void close_fixture(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *dotted) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, dotted, &id);
  return id;
}

static double distance(double a, double b) {
  return a > b ? a - b : b - a;
}

/* A frame off the wire reads back in engineering units, and in raw counts as it always did. */
static int us9_tc01_a_decoded_field_reads_through_its_curve(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  /* board = 0x0320 = 800 -> 800 / 16 - 40 = 10 degrees; bus = 0x800 = 2048 counts. */
  const uint8_t frame[] = { 0x10, 0x03, 0x20, 0x80, 0x00, 0x00, 0x00, 0x00 };
  double celsius = 0;
  double volts = 0;
  uint64_t raw = 0;
  int failure = 0;

  if (framedsl_decode(fixture.frame, frame, sizeof(frame), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_get_eng(fixture.frame, id_of(&fixture, "TM.board"), &celsius) != FRAMEDSL_OK || celsius != 10) {
    failure = __LINE__;
  } else if (framedsl_get_eng(fixture.frame, id_of(&fixture, "TM.bus"), &volts) != FRAMEDSL_OK
             || distance(volts, 2048 * 3.3 / 4095) > 1e-12) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, id_of(&fixture, "TM.board"), &raw) != FRAMEDSL_OK || raw != 800) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* Setting an engineering value stores the raw one, rounded, and that is what goes on the wire. */
static int us9_tc02_an_engineering_value_encodes_as_its_raw_value(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  int failure = 0;
  /* 21.53 C -> (21.53 + 40) * 16 = 984.48 -> 984 = 0x03D8. 1.65 V -> 1.65 * 4095 / 3.3 = 2047.5 ->
   * 2048 = 0x800, rounding half away from zero. */
  if (framedsl_set_eng(fixture.frame, id_of(&fixture, "TM.board"), 21.53) != FRAMEDSL_OK
      || framedsl_set_eng(fixture.frame, id_of(&fixture, "TM.bus"), 1.65) != FRAMEDSL_OK) {
    failure = __LINE__;
  }

  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture.frame, "TM", &length, NULL);
  const uint8_t expected[] = { 0x10, 0x03, 0xD8, 0x80, 0x00, 0x00, 0x00, 0x00 };
  if (failure == 0 && (bytes == NULL || length != sizeof(expected) || memcmp(bytes, expected, length) != 0)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* The tolerance is stated in framedsl.h: half a raw step, which for a line is |scale| / 2. */
static int us9_tc03_a_round_trip_is_within_half_a_raw_step(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const FramedslFieldId board = id_of(&fixture, "TM.board");
  const FramedslFieldId bus = id_of(&fixture, "TM.bus");
  int failure = 0;

  for (double c = -40; c < 4000 && failure == 0; c += 3.217) {
    double back = 0;
    if (framedsl_set_eng(fixture.frame, board, c) != FRAMEDSL_OK
        || framedsl_get_eng(fixture.frame, board, &back) != FRAMEDSL_OK || distance(back, c) > 0.0625 / 2) {
      failure = __LINE__;
    }
  }
  for (double v = 0; v <= 3.3 && failure == 0; v += 0.0137) {
    double back = 0;
    if (framedsl_set_eng(fixture.frame, bus, v) != FRAMEDSL_OK
        || framedsl_get_eng(fixture.frame, bus, &back) != FRAMEDSL_OK || distance(back, v) > 3.3 / 4095 / 2 + 1e-12) {
      failure = __LINE__;
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* A curve that is not a line encodes if it declares an inverse, and is refused if it does not. */
static int us9_tc04_only_an_invertible_curve_can_be_set(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  int failure = 0;
  uint64_t raw = 0;
  /* 10 kOhm -> 4095 * 10000 / 20000 = 2047.5 -> 2048. */
  if (framedsl_set_eng(fixture.frame, id_of(&fixture, "TM.sensor"), 10000) != FRAMEDSL_OK
      || framedsl_get_u64(fixture.frame, id_of(&fixture, "TM.sensor"), &raw) != FRAMEDSL_OK || raw != 2048) {
    failure = __LINE__;
  } else if (framedsl_set_eng(fixture.frame, id_of(&fixture, "TM.area"), 49) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  }

  /* And the definition says which is which before anybody tries. */
  size_t packet = 0;
  FramedslField sensor;
  FramedslField area;
  FramedslField plain;
  if (failure == 0
      && (!framedsl_find_packet(fixture.definition, "TM", &packet)
          || !framedsl_field(fixture.definition, packet, 3, &sensor) || !framedsl_field(fixture.definition, packet, 4, &area)
          || !framedsl_field(fixture.definition, packet, 5, &plain))) {
    failure = __LINE__;
  }
  if (failure == 0
      && (strcmp(sensor.cal, "ohms") != 0 || !sensor.cal_invertible || strcmp(area.cal, "square") != 0
          || area.cal_invertible || plain.cal != NULL || plain.cal_invertible)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A value whose raw counterpart the field cannot hold is refused, and leaves the field alone. */
static int us9_tc05_an_engineering_value_out_of_reach_is_refused(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const FramedslFieldId board = id_of(&fixture, "TM.board");
  int failure = 0;
  uint64_t raw = 0;
  const double refused[] = { -41, 4096.0, 1e300 };

  if (framedsl_set_u64(fixture.frame, board, 1234) != FRAMEDSL_OK) {
    failure = __LINE__;
  }
  for (size_t i = 0; i < sizeof(refused) / sizeof(*refused) && failure == 0; ++i) {
    if (framedsl_set_eng(fixture.frame, board, refused[i]) != FRAMEDSL_ERROR_RANGE) {
      failure = __LINE__;
    }
  }
  if (failure == 0 && (framedsl_get_u64(fixture.frame, board, &raw) != FRAMEDSL_OK || raw != 1234)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A field with no curve is untouched by all of this: no engineering value, raw access as before. */
static int us9_tc06_a_field_without_a_curve_is_unaffected(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const FramedslFieldId plain = id_of(&fixture, "TM.plain");
  double eng = 0;
  uint64_t raw = 0;
  int failure = 0;

  if (framedsl_get_eng(fixture.frame, plain, &eng) != FRAMEDSL_ERROR_FIELD
      || framedsl_set_eng(fixture.frame, plain, 1) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, plain, 200) != FRAMEDSL_OK
             || framedsl_get_u64(fixture.frame, plain, &raw) != FRAMEDSL_OK || raw != 200) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A curve by name, with no field involved: what a program uses to convert a limit or a threshold. */
static int us9_tc07_a_calibration_is_called_by_name(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  FramedslCalId current;
  FramedslCalId square;
  double amps = 0;
  double raw = 0;
  int failure = 0;

  if (framedsl_cal_count(fixture.definition) != 5 || strcmp(framedsl_cal_name(fixture.definition, 1), "current") != 0) {
    failure = __LINE__;
  } else if (!framedsl_find_cal(fixture.definition, "current", &current)
             || !framedsl_find_cal(fixture.definition, "square", &square)
             || framedsl_find_cal(fixture.definition, "nope", NULL)) {
    failure = __LINE__;
  } else if (framedsl_calibrate(fixture.definition, current, 4095, &amps) != FRAMEDSL_OK || distance(amps, 66) > 1e-9) {
    /* A call inlined into a line: the full-scale reading, 3.3 V across 50 mOhm. */
    failure = __LINE__;
  } else if (framedsl_uncalibrate(fixture.definition, current, 33, &raw) != FRAMEDSL_OK
             || distance(raw, 2047.5) > 1e-9) {
    /* Unrounded: only a field knows it holds an integer. */
    failure = __LINE__;
  } else if (framedsl_uncalibrate(fixture.definition, square, 4, &raw) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  }

  /* An id is bound to its definition, the same as a field id. */
  FramedslResult *other = framedsl_load_string(SOURCE, "other.fdsl");
  if (failure == 0 && framedsl_calibrate(framedsl_definition(other), current, 1, &amps) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  }
  framedsl_result_free(other);

  close_fixture(&fixture);
  return failure;
}

/* A divisor that is zero for one raw value is not an error in the file, but that reading is refused
 * rather than returned as infinity. */
static int us9_tc08_a_reading_the_curve_cannot_give_is_refused(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const FramedslFieldId sensor = id_of(&fixture, "TM.sensor");
  double ohms = 0;
  int failure = 0;
  if (framedsl_set_u64(fixture.frame, sensor, 4095) != FRAMEDSL_OK
      || framedsl_get_eng(fixture.frame, sensor, &ohms) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* Reading and writing through a curve is in the hot path, so it allocates as little as encoding does:
 * nothing. */
static int us9_tc09_engineering_access_does_not_allocate(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const FramedslFieldId board = id_of(&fixture, "TM.board");
  const FramedslFieldId sensor = id_of(&fixture, "TM.sensor");
  double eng = 0;

  framedsl_allocation_count_start();
  for (int i = 0; i < 100; ++i) {
    framedsl_set_eng(fixture.frame, board, 20.0 + i);
    framedsl_get_eng(fixture.frame, board, &eng);
    framedsl_set_eng(fixture.frame, sensor, 1000.0 * i);
    framedsl_get_eng(fixture.frame, sensor, &eng);
  }
  size_t allocations = framedsl_allocation_count_stop();

  close_fixture(&fixture);
  return allocations == 0 ? 0 : __LINE__;
}

/* ---- The generated C ----------------------------------------------------------------------------
 *
 * The claim is exact agreement, not closeness: the header is generated from the compiled curves, so a
 * program built against it must print, bit for bit, what the library computes. Checked by compiling
 * the header with the compiler that built this test and comparing hex-float output. */

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

static int us9_tc10_the_emitted_c_computes_what_the_library_does(const TestArguments *args) {
  Fixture fixture;
  if (!open_fixture(&fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  /* snprintf-style: ask the size, then fill. */
  size_t length = framedsl_emit_c(fixture.definition, "gen_", NULL, 0);
  char *header = malloc(length + 1);
  if (length == 0 || header == NULL || framedsl_emit_c(fixture.definition, "gen_", header, length + 1) != length) {
    free(header);
    close_fixture(&fixture);
    return __LINE__;
  }

  char path[1024];
  snprintf(path, sizeof(path), "%s/curves.h", args->tmp_dir);
  FILE *file = fopen(path, "w");
  if (file != NULL) {
    fputs(header, file);
    fclose(file);
  }
  free(header);

  /* No inverse is generated for `square`, which has none. */
  const double raws[] = { 0, 1, 7, 800, 2047, 2048, 3000, 4094 };
  snprintf(path, sizeof(path), "%s/main.c", args->tmp_dir);
  file = fopen(path, "w");
  if (file == NULL) {
    close_fixture(&fixture);
    return __LINE__;
  }
  fputs("#include <stdio.h>\n#include \"curves.h\"\nint main(void) {\n", file);
  for (size_t i = 0; i < sizeof(raws) / sizeof(*raws); ++i) {
    fprintf(
      file,
      "  printf(\"%%a %%a %%a %%a %%a\\n\", gen_volts(%.17g), gen_current(%.17g), gen_celsius(%.17g), gen_ohms(%.17g), "
      "gen_square(%.17g));\n",
      raws[i],
      raws[i],
      raws[i],
      raws[i],
      raws[i]
    );
    fprintf(
      file,
      "  printf(\"%%a %%a %%a\\n\", gen_volts_inverse(%.17g), gen_celsius_inverse(%.17g), gen_ohms_inverse(%.17g));\n",
      raws[i] / 1000,
      raws[i] / 100,
      raws[i] * 3
    );
  }
  fputs("  return 0;\n}\n", file);
  fclose(file);

  char command[4096];
  snprintf(
    command,
    sizeof(command),
    "cc -std=c11 -O2 -Wall -Werror -o %s/curves %s/main.c 2>&1 && %s/curves",
    args->tmp_dir,
    args->tmp_dir,
    args->tmp_dir
  );
  static char output[16384];
  if (run_capture(command, output, sizeof(output)) != 0) {
    close_fixture(&fixture);
    return __LINE__;
  }

  /* The same numbers from the library, in the same format. */
  static char expected[16384];
  size_t used = 0;
  const char *forward[] = { "volts", "current", "celsius", "ohms", "square" };
  const char *backward[] = { "volts", "celsius", "ohms" };
  const double scale[] = { 1.0 / 1000, 1.0 / 100, 3 };
  int failure = 0;

  for (size_t i = 0; i < sizeof(raws) / sizeof(*raws) && failure == 0; ++i) {
    for (size_t c = 0; c < 5; ++c) {
      FramedslCalId cal;
      double value = 0;
      framedsl_find_cal(fixture.definition, forward[c], &cal);
      if (framedsl_calibrate(fixture.definition, cal, raws[i], &value) != FRAMEDSL_OK) {
        failure = __LINE__;
      }
      used += (size_t)snprintf(expected + used, sizeof(expected) - used, c == 0 ? "%a" : " %a", value);
    }
    used += (size_t)snprintf(expected + used, sizeof(expected) - used, "\n");
    for (size_t c = 0; c < 3; ++c) {
      FramedslCalId cal;
      double value = 0;
      framedsl_find_cal(fixture.definition, backward[c], &cal);
      /* Mirrors the arguments written into main.c: raw / 1000, raw / 100, raw * 3. */
      double eng = c == 2 ? raws[i] * scale[c] : raws[i] / (c == 0 ? 1000.0 : 100.0);
      if (framedsl_uncalibrate(fixture.definition, cal, eng, &value) != FRAMEDSL_OK) {
        failure = __LINE__;
      }
      used += (size_t)snprintf(expected + used, sizeof(expected) - used, c == 0 ? "%a" : " %a", value);
    }
    used += (size_t)snprintf(expected + used, sizeof(expected) - used, "\n");
  }

  if (failure == 0 && strcmp(output, expected) != 0) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* ---- At the terminal ---------------------------------------------------------------------------- */

static int us9_tc11_the_tool_shows_and_emits_calibrations(const TestArguments *args) {
  (void)args;
  static char output[16384];
  char command[1024];

  /* board = 800 -> 10 degrees, shown next to the raw value with the curve's name. */
  snprintf(
    command,
    sizeof(command),
    "%s %s/calibrated_sensor.fdsl -d '10 80 07 FF 03 20 07 FF' 2>&1",
    FRAMEDSL_BIN,
    EXAMPLES_DIR
  );
  if (run_capture(command, output, sizeof(output)) != 0 || strstr(output, "800 (0x320) = 10 [celsius]") == NULL) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s/calibrated_sensor.fdsl --emit-c 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR);
  if (run_capture(command, output, sizeof(output)) != 0
      || strstr(output, "static inline double fdsl_celsius(double raw)") == NULL
      || strstr(output, "fdsl_ohms_inverse(double eng)") == NULL) {
    return __LINE__;
  }

  snprintf(command, sizeof(command), "%s %s/calibrated_sensor.fdsl --emit-c -d 00 2>&1", FRAMEDSL_BIN, EXAMPLES_DIR);
  if (run_capture(command, output, sizeof(output)) != 2) {
    return __LINE__;
  }
  return 0;
}

/* ---- Beside the orders (#118) --------------------------------------------------------------------
 *
 * The arena holds a field's raw value in host order, and the curve runs on that; the orders apply only
 * where the arena meets the wire. So a curve must not care what orders its field has, or where they
 * came from. Each case declares two packets that differ only in `@cal` on `v` -- CAL (id 1) and RAW
 * (id 2) -- and sets the engineering value on one and the raw value on the other: the frames must agree
 * after the id byte, and be what the orders alone say they are. */

static const char *const CURVES =
  "cal celsius(raw) = raw * 0.0625 - 40;\n"
  "cal ohms(raw)    = 10000 * raw / (4095 - raw) inverse (eng) = 4095 * eng / (10000 + eng);\n";

/* Whether CAL.v set to `eng` and RAW.v set to `raw` both encode to `want` after the id byte, and CAL's
 * frame decodes back to that raw value and that engineering value. */
static int same_as_raw(const char *source, double eng, uint64_t raw, const uint8_t *want, size_t size) {
  FramedslResult *result = framedsl_load_string(source, "orders.fdsl");
  if (!framedsl_ok(result)) {
    framedsl_result_free(result);
    return __LINE__;
  }
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *calibrated = framedsl_frame_new(definition);
  FramedslFrame *plain = framedsl_frame_new(definition);
  FramedslFieldId cal_v = { 0 };
  FramedslFieldId raw_v = { 0 };
  uint8_t frame[16] = { 0 };
  size_t length = 0;
  int failure = 0;

  if (calibrated == NULL || plain == NULL || !framedsl_find(definition, "CAL.v", &cal_v)
      || !framedsl_find(definition, "RAW.v", &raw_v)) {
    failure = __LINE__;
  } else if (framedsl_set_eng(calibrated, cal_v, eng) != FRAMEDSL_OK
             || framedsl_set_u64(plain, raw_v, raw) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    const uint8_t *bytes = framedsl_encode(calibrated, "CAL", &length, NULL);
    if (bytes == NULL || length != size + 1 || length > sizeof(frame) || memcmp(bytes + 1, want, size) != 0) {
      failure = __LINE__;
    } else {
      memcpy(frame, bytes, length);
      bytes = framedsl_encode(plain, "RAW", &length, NULL);
      if (bytes == NULL || length != size + 1 || memcmp(bytes + 1, want, size) != 0) {
        failure = __LINE__;
      }
    }
  }

  uint64_t got = 0;
  double back = 0;
  if (failure == 0) {
    framedsl_frame_reset(calibrated);
    const char *name = framedsl_decode(calibrated, frame, size + 1, NULL, NULL);
    if (name == NULL || strcmp(name, "CAL") != 0 || framedsl_get_u64(calibrated, cal_v, &got) != FRAMEDSL_OK
        || got != raw || framedsl_get_eng(calibrated, cal_v, &back) != FRAMEDSL_OK
        || distance(back, eng) > 1e-9 * (back < 0 ? -back : back) + 1e-12) {
      failure = __LINE__;
    }
  }
  framedsl_frame_free(calibrated);
  framedsl_frame_free(plain);
  framedsl_result_free(result);
  return failure;
}

/* CAL and RAW, each `id` then `v: width` behind a group of its own, then `tail` -- what follows `v`. */
static void two_packets(
  char *out,
  size_t size,
  const char *cal_group,
  const char *raw_group,
  int width,
  const char *tail
) {
  snprintf(
    out,
    size,
    "%spkt CAL {\n  id: 8 := 1;\n  %s\n  v: %d;\n%s};\npkt RAW {\n  id: 8 := 2;\n  %s\n  v: %d;\n%s};\n",
    CURVES,
    cal_group,
    width,
    tail,
    raw_group,
    width,
    tail
  );
}

/* Every pair of orders in the curve's own group -- on one line, in either order, or over several -- for
 * a line (celsius: 800 counts is 10 degrees) and a curve with an inverse (ohms: 1000 counts); and a bit
 * order alone beside the curve on a field with no bytes to reorder. */
static int us9_tc12_the_orders_in_the_curves_group_change_only_the_wire(const TestArguments *args) {
  (void)args;
  static const char *const bits[] = { "msb_first", "msb_first", "lsb_first", "lsb_first" };
  static const char *const bytes[] = { "big_endian", "little_endian", "big_endian", "little_endian" };
  /* 800 = 0x0320 and 1000 = 0x03E8, as each pair of orders lays them out. */
  static const uint8_t want[2][4][2] = {
    { { 0x03, 0x20 }, { 0x20, 0x03 }, { 0x04, 0xC0 }, { 0xC0, 0x04 } },
    { { 0x03, 0xE8 }, { 0xE8, 0x03 }, { 0x17, 0xC0 }, { 0xC0, 0x17 } },
  };
  static const char *const curves[] = { "celsius", "ohms" };
  static const uint64_t raws[] = { 800, 1000 };

  FramedslResult *loaded = framedsl_load_string(CURVES, "curves.fdsl");
  FramedslCalId ohms;
  double ohms_eng = 0;
  if (!framedsl_find_cal(framedsl_definition(loaded), "ohms", &ohms)
      || framedsl_calibrate(framedsl_definition(loaded), ohms, 1000, &ohms_eng) != FRAMEDSL_OK) {
    framedsl_result_free(loaded);
    return __LINE__;
  }
  framedsl_result_free(loaded);
  const double engs[] = { 10.0, ohms_eng };

  for (int order = 0; order < 4; ++order) {
    char raw_group[128];
    snprintf(raw_group, sizeof(raw_group), "[ @bit_order: %s; @byte_order: %s; ]", bits[order], bytes[order]);
    for (int curve = 0; curve < 2; ++curve) {
      char layouts[3][256];
      snprintf(
        layouts[0],
        sizeof(layouts[0]),
        "[ @cal: %s; @bit_order: %s; @byte_order: %s; ]",
        curves[curve],
        bits[order],
        bytes[order]
      );
      snprintf(
        layouts[1],
        sizeof(layouts[1]),
        "[ @byte_order: %s; @bit_order: %s; @cal: %s; ]",
        bytes[order],
        bits[order],
        curves[curve]
      );
      snprintf(
        layouts[2],
        sizeof(layouts[2]),
        "[\n    @bit_order:  %s;\n    @cal:        %s;\n    @byte_order: %s;\n  ]",
        bits[order],
        curves[curve],
        bytes[order]
      );
      for (int layout = 0; layout < 3; ++layout) {
        char source[2048];
        two_packets(source, sizeof(source), layouts[layout], raw_group, 16, "");
        int failure = same_as_raw(source, engs[curve], raws[curve], want[curve][order], 2);
        if (failure != 0) {
          return failure * 100 + order * 10 + curve * 3 + layout;
        }
      }
    }
  }

  /* A field that is not a whole number of bytes takes a bit order beside its curve, and no byte order.
   * 800 = 0x320 in 12 bits, then 4 bits of `pad`: msb_first is 0x32 0x00, lsb_first reverses the 12. */
  static const uint8_t msb[] = { 0x32, 0x00 };
  static const uint8_t lsb[] = { 0x04, 0xC0 };
  char source[2048];
  two_packets(
    source, sizeof(source), "[ @cal: celsius; @bit_order: msb_first; ]", "[ @bit_order: msb_first; ]", 12, "  pad: 4;\n"
  );
  int failure = same_as_raw(source, 10.0, 800, msb, sizeof(msb));
  if (failure != 0) {
    return failure * 100 + 90;
  }
  two_packets(
    source, sizeof(source), "[ @bit_order: lsb_first; @cal: celsius; ]", "[ @bit_order: lsb_first; ]", 12, "  pad: 4;\n"
  );
  failure = same_as_raw(source, 10.0, 800, lsb, sizeof(lsb));
  return failure != 0 ? failure * 100 + 91 : 0;
}

/* Orders that reach the field from further out -- a packet, an anonymous block, a named block laid out
 * where it is declared, or a field overriding its packet beside the curve -- are the same to the curve. */
static int us9_tc13_orders_from_an_enclosing_scope_are_the_same_to_the_curve(const TestArguments *args) {
  (void)args;
  static const uint8_t little_lsb[] = { 0xC0, 0x04 };
  static const uint8_t big_msb[] = { 0x03, 0x20 };
  char source[2048];
  struct {
    const char *text;
    const uint8_t *want;
  } cases[4];

  static const char packet_group[] =
    "[ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "pkt CAL {\n  id: 8 := 1;\n  [ @cal: celsius; ]\n  v: 16;\n};\n"
    "[ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "pkt RAW {\n  id: 8 := 2;\n  v: 16;\n};\n";
  static const char block_group[] =
    "pkt CAL {\n  id: 8 := 1;\n  [ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "  block {\n    [ @cal: celsius; ]\n    v: 16;\n  };\n};\n"
    "pkt RAW {\n  id: 8 := 2;\n  [ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "  block {\n    v: 16;\n  };\n};\n";
  /* Pasted into packets whose own group says the opposite, which the blocks' fields do not take. */
  static const char named_block[] =
    "[ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "block Calibrated {\n  [ @cal: celsius; ]\n  v: 16;\n};\n"
    "[ @bit_order: lsb_first; @byte_order: little_endian; ]\n"
    "block Plain {\n  v: 16;\n};\n"
    "pkt CAL {\n  id: 8 := 1;\n  Calibrated;\n};\n"
    "pkt RAW {\n  id: 8 := 2;\n  Plain;\n};\n";
  static const char field_override[] =
    "[ @byte_order: little_endian; ]\n"
    "pkt CAL {\n  id: 8 := 1;\n  [ @byte_order: big_endian; @cal: celsius; ]\n  v: 16;\n};\n"
    "[ @byte_order: little_endian; ]\n"
    "pkt RAW {\n  id: 8 := 2;\n  [ @byte_order: big_endian; ]\n  v: 16;\n};\n";
  cases[0].text = packet_group;
  cases[0].want = little_lsb;
  cases[1].text = block_group;
  cases[1].want = little_lsb;
  cases[2].text = named_block;
  cases[2].want = little_lsb;
  cases[3].text = field_override;
  cases[3].want = big_msb;

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    snprintf(source, sizeof(source), "%s%s", CURVES, cases[i].text);
    int failure = same_as_raw(source, 10.0, 800, cases[i].want, 2);
    if (failure != 0) {
      return failure * 10 + (int)i;
    }
  }
  return 0;
}

/* Whether loading `source` reports check `check` at `severity`, and -- when `text` is not NULL -- with
 * `text` somewhere in its message. */
static bool reports(const char *source, int check, FramedslSeverity severity, const char *text) {
  FramedslResult *result = framedsl_load_string(source, "orders.fdsl");
  bool found = false;
  for (size_t i = 0; i < framedsl_diagnostic_count(result) && !found; ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    found = diag->check == check && diag->severity == severity && (text == NULL || strstr(diag->message, text) != NULL);
  }
  framedsl_result_free(result);
  return found;
}

/* The rules about orders hold in a group that also carries a curve: byte_order needs bytes to reorder
 * (check 10), one repeating its packet warns (check 31) and the curve still works, and neither kind of
 * attribute makes `@cal` acceptable in front of a packet or a block. */
static int us9_tc14_the_order_rules_hold_beside_a_curve(const TestArguments *args) {
  (void)args;
  if (!reports(
        "cal c(x) = x * 2;\npkt TM {\n  id: 8 := 1;\n  [ @cal: c; @byte_order: little_endian; ]\n  v: 12;\n  pad: "
        "4;\n};\n",
        10,
        FRAMEDSL_SEVERITY_ERROR,
        NULL
      )) {
    return __LINE__;
  }
  if (!reports(
        "cal c(x) = x * 2;\n[ @cal: c; @byte_order: little_endian; ]\npkt TM {\n  id: 8 := 1;\n  v: 16;\n};\n",
        0,
        FRAMEDSL_SEVERITY_ERROR,
        "cannot apply to a packet"
      )) {
    return __LINE__;
  }
  if (!reports(
        "cal c(x) = x * 2;\npkt TM {\n  id: 8 := 1;\n  [ @cal: c; @bit_order: lsb_first; ]\n  block {\n    v: 16;\n  "
        "};\n};\n",
        0,
        FRAMEDSL_SEVERITY_ERROR,
        "cannot apply to a block"
      )) {
    return __LINE__;
  }

  const char *redundant =
    "cal c(x) = x * 2;\n[ @byte_order: little_endian; ]\npkt TM {\n  id: 8 := 1;\n"
    "  [ @cal: c; @byte_order: little_endian; ]\n  v: 16;\n};\n";
  if (!reports(redundant, 31, FRAMEDSL_SEVERITY_WARNING, NULL)) {
    return __LINE__;
  }
  FramedslResult *result = framedsl_load_string(redundant, "orders.fdsl");
  FramedslFrame *frame = framedsl_ok(result) ? framedsl_frame_new(framedsl_definition(result)) : NULL;
  FramedslFieldId v = { 0 };
  const uint8_t wire[] = { 0x01, 0x34, 0x12 };
  double eng = 0;
  int failure = 0;
  if (frame == NULL || !framedsl_find(framedsl_definition(result), "TM.v", &v)
      || framedsl_decode(frame, wire, sizeof(wire), NULL, NULL) == NULL
      || framedsl_get_eng(frame, v, &eng) != FRAMEDSL_OK || eng != 0x1234 * 2) {
    failure = __LINE__;
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* The widest scalar round-trips through a curve in little-endian; the narrowest refuses a value whose
 * raw counterpart it cannot hold, and keeps what it had. */
static int us9_tc15_the_widest_and_narrowest_scalars_carry_a_curve(const TestArguments *args) {
  (void)args;
  static const uint8_t wide[] = { 0x20, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  char source[2048];
  two_packets(
    source, sizeof(source), "[ @cal: celsius; @byte_order: little_endian; ]", "[ @byte_order: little_endian; ]", 64, ""
  );
  int failure = same_as_raw(source, 10.0, 800, wide, sizeof(wide));
  if (failure != 0) {
    return failure;
  }

  snprintf(
    source,
    sizeof(source),
    "%spkt TM {\n  id: 8 := 1;\n  [ @cal: celsius; @bit_order: lsb_first; ]\n  on: 1;\n  pad: 7;\n};\n",
    CURVES
  );
  FramedslResult *result = framedsl_load_string(source, "orders.fdsl");
  FramedslFrame *frame = framedsl_ok(result) ? framedsl_frame_new(framedsl_definition(result)) : NULL;
  FramedslFieldId on = { 0 };
  uint64_t raw = 0;
  /* -39.9375 is raw 1, which fits; 10 degrees is raw 800, which does not. */
  if (frame == NULL || !framedsl_find(framedsl_definition(result), "TM.on", &on)
      || framedsl_set_eng(frame, on, -39.9375) != FRAMEDSL_OK || framedsl_set_eng(frame, on, 10) != FRAMEDSL_ERROR_RANGE
      || framedsl_get_u64(frame, on, &raw) != FRAMEDSL_OK || raw != 1) {
    failure = __LINE__;
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

/* A fixed-value field may carry a curve: nothing is set through it, but it reads through it like any
 * other field -- 0x10 counts is 16 / 16 - 40 = -39 degrees. */
static int us9_tc16_a_fixed_value_reads_through_its_curve(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string(
    "cal celsius(raw) = raw * 0.0625 - 40;\npkt TM {\n  [ @cal: celsius; @bit_order: msb_first; ]\n  id: 8 := 0x10;\n"
    "  v:  8;\n};\n",
    "fixed.fdsl"
  );
  FramedslFrame *frame = framedsl_ok(result) ? framedsl_frame_new(framedsl_definition(result)) : NULL;
  FramedslFieldId id = { 0 };
  const uint8_t wire[] = { 0x10, 0x00 };
  double eng = 0;
  int failure = 0;
  if (frame == NULL || framedsl_diagnostic_count(result) != 0
      || !framedsl_find(framedsl_definition(result), "TM.id", &id)) {
    failure = __LINE__;
  } else if (framedsl_decode(frame, wire, sizeof(wire), NULL, NULL) == NULL
             || framedsl_get_eng(frame, id, &eng) != FRAMEDSL_OK || eng != -39) {
    failure = __LINE__;
  }
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return failure;
}

void register_user_story9(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us9_tc01_a_decoded_field_reads_through_its_curve, "a decoded field reads in engineering units and in raw", true
  );
  ADD_TEST_CASE(
    us9_tc02_an_engineering_value_encodes_as_its_raw_value,
    "an engineering value encodes as its rounded raw value",
    false
  );
  ADD_TEST_CASE(us9_tc03_a_round_trip_is_within_half_a_raw_step, "a round trip is within half a raw step", false);
  ADD_TEST_CASE(us9_tc04_only_an_invertible_curve_can_be_set, "a curve sets only if linear or given an inverse", false);
  ADD_TEST_CASE(
    us9_tc05_an_engineering_value_out_of_reach_is_refused, "a value the field cannot hold is refused", false
  );
  ADD_TEST_CASE(us9_tc06_a_field_without_a_curve_is_unaffected, "a field without a curve is unaffected", false);
  ADD_TEST_CASE(
    us9_tc07_a_calibration_is_called_by_name, "a calibration is called by name, bound to its definition", false
  );
  ADD_TEST_CASE(
    us9_tc08_a_reading_the_curve_cannot_give_is_refused, "a reading that is not a finite number is refused", false
  );
  ADD_TEST_CASE(
    us9_tc09_engineering_access_does_not_allocate, "reading and writing through a curve allocates nothing", false
  );
  ADD_TEST_CASE(
    us9_tc10_the_emitted_c_computes_what_the_library_does, "the emitted C agrees with the library bit for bit", false
  );
  ADD_TEST_CASE(
    us9_tc11_the_tool_shows_and_emits_calibrations, "-d shows engineering values and --emit-c writes C", false
  );
  ADD_TEST_CASE(
    us9_tc12_the_orders_in_the_curves_group_change_only_the_wire,
    "orders in the curve's group change only the wire",
    false
  );
  ADD_TEST_CASE(
    us9_tc13_orders_from_an_enclosing_scope_are_the_same_to_the_curve,
    "inherited orders are the same to the curve",
    false
  );
  ADD_TEST_CASE(us9_tc14_the_order_rules_hold_beside_a_curve, "the order rules hold beside a curve", false);
  ADD_TEST_CASE(
    us9_tc15_the_widest_and_narrowest_scalars_carry_a_curve, "64-bit and 1-bit fields carry a curve", false
  );
  ADD_TEST_CASE(us9_tc16_a_fixed_value_reads_through_its_curve, "a fixed-value field reads through its curve", false);
}
