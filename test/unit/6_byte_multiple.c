/* Story 6: check 13 -- a packet is a whole number of bytes.
 *
 * Split from story 3 because it is two things rather than one. Summing constant widths is an
 * addition; deciding whether a valof() width is a multiple of eight is an analysis, and the
 * interesting cases are all in the second half. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

/* Parses and checks `source`. Reports whether check `wanted` fired, and hands back the message it
 * fired with -- `found_message` points into `diags`, so it is only read before this returns. */
static bool rejected_by(const char *source, int wanted, char *message, size_t message_size) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  bool found = false;

  if (diags.count == 0 && file != NULL) {
    Layout *layout = check_file(file, &diags);
    for (size_t i = 0; i < diags.count; ++i) {
      if (diags.items[i].check == wanted) {
        found = true;
        if (message != NULL) {
          snprintf(message, message_size, "%s", diags.items[i].message);
        }
      }
    }
    layout_free(layout);
  }

  ast_free(file);
  diag_free(&diags);
  return found;
}

static bool rejects(const char *source) {
  return rejected_by(source, 13, NULL, 0);
}

/* Loads cleanly: no diagnostic at all, not merely no check-13 diagnostic. A source that trips some
 * other check while under test for this one is a test that stopped measuring what it names. */
static bool accepts(const char *source) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  Layout *layout = NULL;
  if (diags.count == 0 && file != NULL) {
    layout = check_file(file, &diags);
  }

  bool clean = layout != NULL && diags.count == 0;

  layout_free(layout);
  ast_free(file);
  diag_free(&diags);
  return clean;
}

static int us6_tc01_accepts_a_packet_that_is_a_whole_number_of_bytes(const TestArguments *args) {
  (void)args;
  /* Every field is sub-byte and two of them straddle a byte boundary. The total is what matters. */
  if (!accepts("pkt A { v: 3 := 0b000; t: 1 := 0b0; f: 1; id: 11; s: 2; n: 14; len: 16; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us6_tc02_rejects_a_packet_that_is_not(const TestArguments *args) {
  (void)args;
  if (!rejects("pkt A { base: 11 := 0; rtr: 1; ide: 1; r0: 1; dlc: 4; d: 8; seq: 15; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The author should not have to add up their own fields to learn how far off they are. */
static int us6_tc03_reports_the_total_for_a_constant_width_packet(const TestArguments *args) {
  (void)args;
  char message[512] = { 0 };
  if (!rejected_by(
        "pkt A { base: 11 := 0; rtr: 1; ide: 1; r0: 1; dlc: 4; d: 8; seq: 15; };", 13, message, sizeof(message)
      )) {
    FORCE_FAILURE();
  }
  if (strstr(message, "41 bits") == NULL) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us6_tc04_accepts_a_valof_width_in_bytes(const TestArguments *args) {
  (void)args;
  if (!accepts("pkt A { h: 8 := 0xAA; n: 16; d[256]: valof(n) * 8; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* One factor being a multiple of eight settles the product, whatever the other factor is. This is
 * the CCSDS shape: a length field that counts octets minus one. */
static int us6_tc05_accepts_a_product_whose_other_factor_is_unknown(const TestArguments *args) {
  (void)args;
  if (!accepts("pkt A { h: 8 := 0xAA; n: 16; d[256]: (valof(n) + 1) * 8; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The analysis is not a search for a literal `* 8`: a sum of two byte multiples is one too. */
static int us6_tc06_accepts_a_byte_multiple_built_by_arithmetic(const TestArguments *args) {
  (void)args;
  if (!accepts("pkt A { h: 8 := 0xAA; n: 16; d[256]: valof(n) * 16 - 32; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us6_tc07_rejects_a_valof_width_that_is_not_a_byte_multiple(const TestArguments *args) {
  (void)args;
  if (!rejects("pkt A { h: 8 := 0xAA; n: 16; d[384]: valof(n) * 3; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* Conservative on purpose, and documented as such in docs/design.md: no single factor here is a
 * multiple of eight, so the product is rejected even though 2 * 4 always is. Pinned by a test
 * because it is a deliberate limit rather than an oversight -- if it is ever loosened, this should
 * fail and be deleted on purpose. */
static int us6_tc08_rejects_a_byte_multiple_it_cannot_see(const TestArguments *args) {
  (void)args;
  if (!rejects("pkt A { h: 8 := 0xAA; n: 16; d[384]: valof(n) * 2 * 4; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The other side of that limit: a multiple of eight anywhere in the chain of factors is enough,
 * because the analysis works outwards from each operator rather than looking for a literal. */
static int us6_tc09_accepts_a_product_with_a_byte_multiple_factor(const TestArguments *args) {
  (void)args;
  if (!accepts("pkt A { h: 8 := 0xAA; n: 16; d[384]: valof(n) * 3 * 8; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* A byte-sized variable field does not rescue a constant part that is off by four bits. */
static int us6_tc10_rejects_an_odd_constant_part(const TestArguments *args) {
  (void)args;
  if (!rejects("pkt A { h: 4 := 0xA; n: 16; d[256]: valof(n) * 8; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* A width that already failed check 4 has no residue to speak of, and saying so again as check 13
 * would report one mistake twice. */
static int us6_tc11_does_not_pile_on_after_a_bad_width(const TestArguments *args) {
  (void)args;
  if (rejects("pkt A { h: 8 := 0xAA; x: 4 - 4; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

void register_user_story6(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us6_tc01_accepts_a_packet_that_is_a_whole_number_of_bytes,
    "sub-byte fields summing to a byte multiple are accepted",
    false
  );
  ADD_TEST_CASE(us6_tc02_rejects_a_packet_that_is_not, "a packet that is not a byte multiple is rejected", false);
  ADD_TEST_CASE(
    us6_tc03_reports_the_total_for_a_constant_width_packet, "the diagnostic reports the packet's total width", false
  );
  ADD_TEST_CASE(us6_tc04_accepts_a_valof_width_in_bytes, "valof(n) * 8 is accepted", false);
  ADD_TEST_CASE(us6_tc05_accepts_a_product_whose_other_factor_is_unknown, "(valof(n) + 1) * 8 is accepted", false);
  ADD_TEST_CASE(us6_tc06_accepts_a_byte_multiple_built_by_arithmetic, "valof(n) * 16 - 32 is accepted", false);
  ADD_TEST_CASE(us6_tc07_rejects_a_valof_width_that_is_not_a_byte_multiple, "valof(n) * 3 is rejected", false);
  ADD_TEST_CASE(us6_tc08_rejects_a_byte_multiple_it_cannot_see, "the analysis is conservative, deliberately", false);
  ADD_TEST_CASE(
    us6_tc09_accepts_a_product_with_a_byte_multiple_factor,
    "a byte-multiple factor anywhere in a product is enough",
    false
  );
  ADD_TEST_CASE(
    us6_tc10_rejects_an_odd_constant_part, "a byte-sized variable field does not excuse an odd constant part", false
  );
  ADD_TEST_CASE(
    us6_tc11_does_not_pile_on_after_a_bad_width, "a width that failed check 4 is not reported again as check 13", false
  );
}
