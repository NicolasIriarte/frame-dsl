/* Story 3: the checks a single field or packet can be judged by on its own.
 *
 * Identification (checks 10-12) is story 4; layout and projection are story 5. Split up because
 * CLibs' test suite allows sixteen cases per story, and because these three are separate
 * concerns that happen to share a parser. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

/* Parses and checks `source`, reporting whether check `wanted` fired.
 *
 * Asking for a specific check rather than "was it rejected" is the point: a file rejected for the
 * wrong reason is a passing test hiding a broken checker. */
static bool rejected_by(const char *source, int wanted) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  bool found = false;

  if (diags.count == 0 && file != NULL) {
    Layout *layout = check_file(file, &diags);
    for (size_t i = 0; i < diags.count; ++i) {
      if (diags.items[i].check == wanted) {
        found = true;
      }
    }
    layout_free(layout);
  }

  ast_free(file);
  diag_free(&diags);
  return found;
}

static int us3_tc01_rejects_a_duplicate_packet_name(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; };\npkt A { h: 8 = 2; };", 1)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc02_rejects_a_duplicate_field_name(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; x: 4; x: 4; };", 2)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc03_rejects_a_zero_width_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; x: 4 - 4; };", 4)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc04_rejects_a_valof_width_without_a_reservation(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; n: 16; d: valof(n) * 8; };", 5)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc05_rejects_a_reservation_that_is_not_constant(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; n: 16; d[valof(n)]: valof(n) * 8; };", 6)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc06_rejects_a_valof_naming_a_later_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; d[8]: valof(n) * 8; n: 16; };", 7)) {
    FORCE_FAILURE();
  }
  return 0;
}

/* A field in another packet is not in scope: each packet is decoded on its own. */
static int us3_tc07_rejects_a_valof_naming_another_packets_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; n: 16; };\npkt B { h: 8 = 2; d[8]: valof(n) * 8; };", 7)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc08_rejects_a_fixed_value_too_wide_for_its_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { k: 4 = 0xFF; b: 16; };", 8)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us3_tc09_rejects_a_field_with_both_fixed_value_and_max(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; n: 16; d[8]: valof(n) * 8 = 3; };", 9)) {
    FORCE_FAILURE();
  }
  return 0;
}

void register_user_story3(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us3_tc01_rejects_a_duplicate_packet_name, "check 1: duplicate packet name", false);
  ADD_TEST_CASE(us3_tc02_rejects_a_duplicate_field_name, "check 2: duplicate field name", false);
  ADD_TEST_CASE(us3_tc03_rejects_a_zero_width_field, "check 4: a width must be at least one bit", false);
  ADD_TEST_CASE(us3_tc04_rejects_a_valof_width_without_a_reservation, "check 5: a valof() width needs a reservation", false);
  ADD_TEST_CASE(us3_tc05_rejects_a_reservation_that_is_not_constant,
                "check 6: a reservation must be constant", false);
  ADD_TEST_CASE(us3_tc06_rejects_a_valof_naming_a_later_field, "check 7: valof() must look backwards", false);
  ADD_TEST_CASE(us3_tc07_rejects_a_valof_naming_another_packets_field, "check 7: valof() cannot cross packets", false);
  ADD_TEST_CASE(us3_tc08_rejects_a_fixed_value_too_wide_for_its_field, "check 8: a fixed value must fit its width", false);
  ADD_TEST_CASE(us3_tc09_rejects_a_field_with_both_fixed_value_and_max, "check 9: a field cannot be both variable and a discriminator", false);
}
