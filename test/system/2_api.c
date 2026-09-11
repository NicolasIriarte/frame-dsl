/* Story 2: using Frame-DSL as a library.
 *
 * A system test, so the only header from this project is the public one. If anything here needed an
 * internal header the interface would not be finished -- and it could not get one anyway, since
 * src/ is not on this file's include path. */
#include <stdio.h>
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

#define DEFINITION_A                                                                               \
  "pkt TM_A {\n"                                                                                   \
  "  hdr: 8 = 0xAA;\n"                                                                             \
  "  len: 32;\n"                                                                                   \
  "  data: valof(len) * 8 max 1024;\n"                                                             \
  "  crc: 16;\n"                                                                                   \
  "};\n"

#define DEFINITION_B "pkt TM_B { hdr: 8 = 0xBB; seq: 16; };\n"

/* Nothing is global, so a program can speak two protocols at once. This is the case that would have
 * been impossible under the original sketch, where the API had no handle at all. */
static int us2_tc01_two_definitions_can_be_open_at_once(const TestArguments *args) {
  (void)args;

  FramedslResult *a = framedsl_load_string(DEFINITION_A, "a.fdsl");
  FramedslResult *b = framedsl_load_string(DEFINITION_B, "b.fdsl");

  int failure = 0;
  if (!framedsl_ok(a) || !framedsl_ok(b)) {
    failure = __LINE__;
  } else if (strcmp(framedsl_packet_name(framedsl_definition(a), 0), "TM_A") != 0) {
    failure = __LINE__;
  } else if (strcmp(framedsl_packet_name(framedsl_definition(b), 0), "TM_B") != 0) {
    failure = __LINE__;
  }

  /* And freeing one must leave the other entirely usable. */
  framedsl_result_free(a);
  if (failure == 0) {
    const FramedslDefinition *still = framedsl_definition(b);
    if (framedsl_packet_count(still) != 1 || strcmp(framedsl_packet_name(still, 0), "TM_B") != 0) {
      failure = __LINE__;
    }
  }
  framedsl_result_free(b);
  return failure;
}

static int us2_tc02_reports_every_problem_not_only_the_first(const TestArguments *args) {
  (void)args;

  /* Two packets, each with its own mistake: a fixed value too wide, and no discriminator at all. */
  FramedslResult *result = framedsl_load_string("pkt A { k: 4 = 0xFF; b: 16; };\n"
                                                "pkt B { x: 8; y: 8; };\n",
                                                "two.fdsl");

  int failure = 0;
  if (framedsl_status(result) != FRAMEDSL_ERROR_INVALID) {
    failure = __LINE__;
  } else if (framedsl_diagnostic_count(result) < 2) {
    failure = __LINE__;
  } else {
    bool saw_8 = false;
    bool saw_11 = false;
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      int check = framedsl_diagnostic(result, i)->check;
      saw_8 = saw_8 || check == 8;
      saw_11 = saw_11 || check == 11;
    }
    if (!saw_8 || !saw_11) {
      failure = __LINE__;
    }
  }

  framedsl_result_free(result);
  return failure;
}

static int us2_tc03_a_diagnostic_carries_a_position(const TestArguments *args) {
  (void)args;

  FramedslResult *result = framedsl_load_string("pkt A {\n  hdr 8;\n};\n", "bad.fdsl");

  int failure = 0;
  if (framedsl_ok(result) || framedsl_diagnostic_count(result) == 0) {
    failure = __LINE__;
  } else {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, 0);
    if (diag->line != 2 || diag->column != 7 || diag->message == NULL) {
      failure = __LINE__;
    } else if (diag->check != 0) {
      failure = __LINE__; /* a syntax error belongs to no numbered check */
    }
  }

  framedsl_result_free(result);
  return failure;
}

/* A file that is not there is a different problem from a file that is wrong, and a caller should not
 * have to read the message to tell them apart. */
static int us2_tc04_a_missing_file_is_not_an_invalid_one(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/absent.fdsl", args->tmp_dir);

  FramedslResult *missing = framedsl_load_file(path);
  FramedslResult *invalid = framedsl_load_string("pkt A { x: 8; };", "invalid.fdsl");

  int failure = 0;
  if (framedsl_status(missing) != FRAMEDSL_ERROR_IO) {
    failure = __LINE__;
  } else if (framedsl_status(invalid) != FRAMEDSL_ERROR_INVALID) {
    failure = __LINE__;
  } else if (framedsl_definition(missing) != NULL || framedsl_definition(invalid) != NULL) {
    failure = __LINE__;
  }

  framedsl_result_free(missing);
  framedsl_result_free(invalid);
  return failure;
}

static int us2_tc05_finds_packets_and_fields_by_name(const TestArguments *args) {
  (void)args;

  FramedslResult *result = framedsl_load_string(DEFINITION_A, "a.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);

  int failure = 0;
  size_t packet = 0;
  size_t field = 0;

  if (!framedsl_find_packet(definition, "TM_A", &packet)) {
    failure = __LINE__;
  } else if (!framedsl_find_field(definition, packet, "crc", &field) || field != 3) {
    failure = __LINE__;
  } else if (framedsl_find_packet(definition, "NOPE", &packet)) {
    failure = __LINE__; /* a miss is reported, not answered with a valid-looking zero */
  } else if (framedsl_find_field(definition, 0, "nope", &field)) {
    failure = __LINE__;
  }

  framedsl_result_free(result);
  return failure;
}

static int us2_tc06_describes_a_variable_field_as_variable(const TestArguments *args) {
  (void)args;

  FramedslResult *result = framedsl_load_string(DEFINITION_A, "a.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);

  int failure = 0;
  FramedslField data;
  FramedslField crc;

  if (!framedsl_field(definition, 0, 2, &data) || !framedsl_field(definition, 0, 3, &crc)) {
    failure = __LINE__;
  } else if (!data.variable || data.max_bits != 1024 || data.width_bits != 0) {
    failure = __LINE__; /* a bound, not a bogus width */
  } else if (data.reserved_bits != 1024) {
    failure = __LINE__; /* the arena reserves the bound */
  } else if (crc.offset_known) {
    failure = __LINE__; /* crc follows a variable field, so nobody knows where it starts */
  }

  framedsl_result_free(result);
  return failure;
}

/* The *resolved* setting, not the written one: a caller wants to know what applies to this field,
 * not which of three scopes happened to say so. */
static int us2_tc07_reports_resolved_bit_and_byte_order(const TestArguments *args) {
  (void)args;

  FramedslResult *result = framedsl_load_string("byte_order big_endian;\n"
                                                "pkt P {\n"
                                                "  byte_order little_endian;\n"
                                                "  hdr: 8 = 0x01;\n"
                                                "  a: 32;\n"
                                                "  b: 32 byte_order big_endian;\n"
                                                "  c: 4 bit_order lsb_first;\n"
                                                "};\n",
                                                "orders.fdsl");

  int failure = 0;
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslField a;
  FramedslField b;
  FramedslField c;

  if (!framedsl_ok(result)) {
    failure = __LINE__;
  } else if (!framedsl_field(definition, 0, 1, &a) || !framedsl_field(definition, 0, 2, &b) ||
             !framedsl_field(definition, 0, 3, &c)) {
    failure = __LINE__;
  } else if (a.byte_order != FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN) {
    failure = __LINE__; /* inherited from the packet */
  } else if (b.byte_order != FRAMEDSL_BYTE_ORDER_BIG_ENDIAN) {
    failure = __LINE__; /* written on the field */
  } else if (c.bit_order != FRAMEDSL_BIT_ORDER_LSB_FIRST) {
    failure = __LINE__;
  } else if (c.byte_order != FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN) {
    failure = __LINE__; /* bit order and byte order are independent */
  }

  framedsl_result_free(result);
  return failure;
}

/* Out of range is an answer, not a crash and not a zeroed struct that looks like a real field. */
static int us2_tc08_out_of_range_indices_are_refused(const TestArguments *args) {
  (void)args;

  FramedslResult *result = framedsl_load_string(DEFINITION_A, "a.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);

  FramedslField field;
  memset(&field, 0xEE, sizeof(field));
  FramedslField untouched;
  memset(&untouched, 0xEE, sizeof(untouched));

  int failure = 0;
  if (framedsl_field(definition, 99, 0, &field) || framedsl_field(definition, 0, 99, &field)) {
    failure = __LINE__;
  } else if (memcmp(&field, &untouched, sizeof(field)) != 0) {
    failure = __LINE__; /* a refused call leaves the caller's struct alone */
  } else if (framedsl_packet_name(definition, 99) != NULL) {
    failure = __LINE__;
  } else if (framedsl_field_count(definition, 99) != 0) {
    failure = __LINE__;
  }

  framedsl_result_free(result);
  return failure;
}

/* Every entry point should survive being handed nothing, since a caller that failed to load will
 * have NULL and the interesting question is whether it finds out or crashes. */
static int us2_tc09_null_arguments_are_survivable(const TestArguments *args) {
  (void)args;

  FramedslField field;
  if (framedsl_ok(NULL) || framedsl_definition(NULL) != NULL) {
    FORCE_FAILURE();
  }
  if (framedsl_packet_count(NULL) != 0 || framedsl_field_count(NULL, 0) != 0) {
    FORCE_FAILURE();
  }
  if (framedsl_packet_name(NULL, 0) != NULL || framedsl_diagnostic(NULL, 0) != NULL) {
    FORCE_FAILURE();
  }
  if (framedsl_field(NULL, 0, 0, &field) || framedsl_find_packet(NULL, "x", NULL)) {
    FORCE_FAILURE();
  }
  if (framedsl_source_name(NULL) == NULL) {
    FORCE_FAILURE();
  }
  framedsl_result_free(NULL);
  return 0;
}

static int us2_tc10_loads_the_examples_from_disk(const TestArguments *args) {
  (void)args;

  char path[512];
  snprintf(path, sizeof(path), "%s/packet_family.fdsl", EXAMPLES_DIR);

  FramedslResult *result = framedsl_load_file(path);
  int failure = 0;

  if (!framedsl_ok(result)) {
    failure = __LINE__;
  } else {
    const FramedslDefinition *definition = framedsl_definition(result);
    size_t packet = 0;
    if (framedsl_packet_count(definition) != 3) {
      failure = __LINE__;
    } else if (!framedsl_find_packet(definition, "TC_PAYLOAD", &packet)) {
      failure = __LINE__;
    } else if (framedsl_packet_reserved_bits(definition, packet) != 8 + 16 + 16 + 2048) {
      failure = __LINE__;
    }
  }

  framedsl_result_free(result);
  return failure;
}

void register_user_story2(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us2_tc01_two_definitions_can_be_open_at_once,
                "two definitions load at once and are independent", true);
  ADD_TEST_CASE(us2_tc02_reports_every_problem_not_only_the_first,
                "a failed load reports every problem", false);
  ADD_TEST_CASE(us2_tc03_a_diagnostic_carries_a_position, "a diagnostic carries line and column", false);
  ADD_TEST_CASE(us2_tc04_a_missing_file_is_not_an_invalid_one,
                "a missing file is a different status from a bad one", false);
  ADD_TEST_CASE(us2_tc05_finds_packets_and_fields_by_name, "packets and fields are found by name", false);
  ADD_TEST_CASE(us2_tc06_describes_a_variable_field_as_variable,
                "a variable field reports its bound, not a width", false);
  ADD_TEST_CASE(us2_tc07_reports_resolved_bit_and_byte_order,
                "bit and byte order come back resolved", false);
  ADD_TEST_CASE(us2_tc08_out_of_range_indices_are_refused, "an out-of-range index is refused", false);
  ADD_TEST_CASE(us2_tc09_null_arguments_are_survivable, "every entry point survives NULL", false);
  ADD_TEST_CASE(us2_tc10_loads_the_examples_from_disk, "an example loads from disk and measures up", false);
}
