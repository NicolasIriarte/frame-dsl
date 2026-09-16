/* Story 4: telling packets apart.
 *
 * byte_order applicability (check 10), the required discriminator (11), and distinguishability (12).
 * Separate from story 3 because these are about a file as a whole rather than about one field, and
 * because CLibs' suite allows sixteen cases per story. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

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

static bool accepted(const char *source) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  bool ok = false;

  if (diags.count == 0 && file != NULL) {
    Layout *layout = check_file(file, &diags);
    ok = layout != NULL && diags.count == 0;
    layout_free(layout);
  }

  ast_free(file);
  diag_free(&diags);
  return ok;
}

static int us4_tc01_rejects_byte_order_written_on_a_sub_byte_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 1; [ @byte_order: little_endian; ] s: 15; t: 1; };", 10)) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The distinction a reference implementation already got wrong once: a file-level byte_order covers
 * every field below it, and most real frames contain sub-byte fields. Only a setting *written* where
 * it can do nothing is an error. */
static int us4_tc02_accepts_byte_order_inherited_by_a_sub_byte_field(const TestArguments *args) {
  (void)args;
  if (!accepted("@byte_order: little_endian;\npkt A { h: 8 = 1; s: 15; t: 1; };")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us4_tc03_rejects_byte_order_on_a_field_off_a_byte_boundary(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 4 = 1; [ @byte_order: little_endian; ] w: 16; t: 4; };", 10)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us4_tc04_rejects_a_packet_with_no_fixed_field(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { a: 8; b: 8; };", 11)) {
    FORCE_FAILURE();
  }
  return 0;
}

/* Both have a discriminator, both pin the same bits to the same value: nothing contradicts. */
static int us4_tc05_rejects_packets_pinning_the_same_bits_alike(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 0xAA; n: 16; };\npkt B { h: 8 = 0xAA; m: 32; };", 12)) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The case the informal rule gets wrong. Different discriminators, but at offsets that do not
 * overlap: the masks share no bits, so a frame can satisfy both and the decoder would be guessing. */
static int us4_tc06_rejects_packets_whose_discriminators_do_not_overlap(const TestArguments *args) {
  (void)args;
  if (!rejected_by("pkt A { h: 8 = 0xAA; n: 16; };\npkt B { p: 8; q: 8 = 0xBB; };", 12)) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us4_tc07_accepts_packets_pinning_one_offset_to_different_values(const TestArguments *args) {
  (void)args;
  if (!accepted(
        "pkt A { t: 8 = 0x01; n: 16; };\n"
        "pkt B { t: 8 = 0x02; m: 32; };\n"
        "pkt C { t: 8 = 0x03; k: 8; };"
      )) {
    FORCE_FAILURE();
  }
  return 0;
}

/* An ambiguity is about a pair, so the message has to name the pair -- "this file is ambiguous"
 * leaves the reader to find which two. */
static int us4_tc08_names_both_packets_in_an_ambiguity(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("pkt ALPHA { h: 8 = 0xAA; n: 16; };\npkt BETA { h: 8 = 0xAA; m: 32; };", &diags);

  int failure = 0;
  if (diags.count == 0 && file != NULL) {
    Layout *layout = check_file(file, &diags);
    bool named_both = false;
    for (size_t i = 0; i < diags.count; ++i) {
      if (diags.items[i].check == 12 && strstr(diags.items[i].message, "ALPHA") != NULL
          && strstr(diags.items[i].message, "BETA") != NULL) {
        named_both = true;
      }
    }
    if (!named_both) {
      failure = __LINE__;
    }
    layout_free(layout);
  } else {
    failure = __LINE__;
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

void register_user_story4(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us4_tc01_rejects_byte_order_written_on_a_sub_byte_field,
    "check 10: byte_order written on a field with no bytes",
    false
  );
  ADD_TEST_CASE(
    us4_tc02_accepts_byte_order_inherited_by_a_sub_byte_field,
    "check 10: an inherited byte_order is not an error",
    false
  );
  ADD_TEST_CASE(
    us4_tc03_rejects_byte_order_on_a_field_off_a_byte_boundary,
    "check 10: byte_order on a field off a byte boundary",
    false
  );
  ADD_TEST_CASE(us4_tc04_rejects_a_packet_with_no_fixed_field, "check 11: a packet needs a discriminator", false);
  ADD_TEST_CASE(
    us4_tc05_rejects_packets_pinning_the_same_bits_alike, "check 12: same bits, same values, indistinguishable", false
  );
  ADD_TEST_CASE(
    us4_tc06_rejects_packets_whose_discriminators_do_not_overlap,
    "check 12: discriminators must contradict, not merely exist",
    false
  );
  ADD_TEST_CASE(
    us4_tc07_accepts_packets_pinning_one_offset_to_different_values,
    "check 12: a shared type field tells packets apart",
    false
  );
  ADD_TEST_CASE(us4_tc08_names_both_packets_in_an_ambiguity, "check 12: the message names both packets", false);
}
