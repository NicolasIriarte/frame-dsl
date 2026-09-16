/* Story 5: layout.
 *
 * Where each field sits, what the arena reserves, and which stream bits a fixed value pins once bit
 * order and byte order have been applied. */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

static Layout *layout_of(const char *source, AstFile **out_file) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  Layout *layout = NULL;
  if (diags.count == 0 && file != NULL) {
    layout = check_file(file, &diags);
  }
  *out_file = file;
  diag_free(&diags);
  return layout;
}

static int us5_tc01_assigns_bit_offsets_in_declaration_order(const TestArguments *args) {
  (void)args;

  AstFile *file = NULL;
  Layout *layout = layout_of("pkt A { h: 8 = 1; a: 11; b: 1; c: 4; };", &file);

  int failure = 0;
  if (layout == NULL) {
    failure = __LINE__;
  } else {
    const FieldLayout *fields = layout->packets[0].fields;
    if (fields[0].offset_bits != 0 || fields[1].offset_bits != 8 || fields[2].offset_bits != 19
        || fields[3].offset_bits != 20) {
      failure = __LINE__;
    } else if (layout->packets[0].reserved_bits != 24) {
      failure = __LINE__;
    }
  }

  layout_free(layout);
  ast_free(file);
  return failure;
}

/* Everything after a variable-width field sits somewhere nobody knows until a frame arrives. */
static int us5_tc02_offsets_stop_being_known_after_a_variable_field(const TestArguments *args) {
  (void)args;

  AstFile *file = NULL;
  Layout *layout = layout_of("pkt A { h: 8 = 1; n: 16; d[8]: valof(n) * 8; crc: 16; };", &file);

  int failure = 0;
  if (layout == NULL) {
    failure = __LINE__;
  } else {
    const FieldLayout *fields = layout->packets[0].fields;
    if (!fields[1].offset_known || fields[1].offset_bits != 8) {
      failure = __LINE__;
    } else if (!fields[2].variable || fields[2].max_bits != 64) {
      failure = __LINE__;
    } else if (fields[3].offset_known) {
      failure = __LINE__; /* crc follows a variable field */
    } else if (layout->packets[0].reserved_bits != 8 + 16 + 64 + 16) {
      failure = __LINE__; /* the arena reserves the bound, not the current length */
    }
  }

  layout_free(layout);
  ast_free(file);
  return failure;
}

static int us5_tc03_marks_wide_fields_as_bit_strings(const TestArguments *args) {
  (void)args;

  AstFile *file = NULL;
  /* `pad` is there only to bring the packet to 144 bits: check 13 wants a whole number of bytes. */
  Layout *layout = layout_of("pkt A { h: 8 = 1; small: 64; big: 65; pad: 7; };", &file);

  int failure = 0;
  if (layout == NULL) {
    failure = __LINE__;
  } else {
    const FieldLayout *fields = layout->packets[0].fields;
    if (!fields[1].is_scalar || fields[2].is_scalar) {
      failure = __LINE__;
    }
  }

  layout_free(layout);
  ast_free(file);
  return failure;
}

static int us5_tc04_bit_order_changes_which_stream_bits_are_pinned(const TestArguments *args) {
  (void)args;

  AstFile *msb_file = NULL;
  AstFile *lsb_file = NULL;
  Layout *msb = layout_of("pkt A { h: 8 = 0xAA; };", &msb_file);
  Layout *lsb = layout_of("pkt A { [ @bit_order: lsb_first; ] h: 8 = 0xAA; };", &lsb_file);

  int failure = 0;
  if (msb == NULL || lsb == NULL) {
    failure = __LINE__;
  } else if (msb->packets[0].mask[0] != 0xFF || lsb->packets[0].mask[0] != 0xFF) {
    failure = __LINE__; /* both pin all eight bits */
  } else if (msb->packets[0].value[0] == lsb->packets[0].value[0]) {
    failure = __LINE__; /* but not to the same pattern */
  }

  layout_free(msb);
  layout_free(lsb);
  ast_free(msb_file);
  ast_free(lsb_file);
  return failure;
}

/* A little-endian field pins the same stream bits as a big-endian one holding the byte-swapped
 * value. Written as an equivalence rather than a hand-computed constant, so the case says what the
 * setting *means* instead of restating the implementation back to itself. */
static int us5_tc05_byte_order_swaps_the_bytes_that_are_pinned(const TestArguments *args) {
  (void)args;

  AstFile *little_file = NULL;
  AstFile *big_file = NULL;
  Layout *little = layout_of("pkt A { [ @byte_order: little_endian; ] h: 16 = 0xCAFE; };", &little_file);
  Layout *big = layout_of("pkt A { [ @byte_order: big_endian; ] h: 16 = 0xFECA; };", &big_file);

  int failure = 0;
  if (little == NULL || big == NULL) {
    failure = __LINE__;
  } else if (memcmp(little->packets[0].mask, big->packets[0].mask, 2) != 0) {
    failure = __LINE__;
  } else if (memcmp(little->packets[0].value, big->packets[0].value, 2) != 0) {
    failure = __LINE__;
  }

  layout_free(little);
  layout_free(big);
  ast_free(little_file);
  ast_free(big_file);
  return failure;
}

void register_user_story5(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us5_tc01_assigns_bit_offsets_in_declaration_order, "fields are laid out in declaration order", false);
  ADD_TEST_CASE(
    us5_tc02_offsets_stop_being_known_after_a_variable_field,
    "offsets stop being constant after a variable field",
    false
  );
  ADD_TEST_CASE(us5_tc03_marks_wide_fields_as_bit_strings, "over 64 bits is a bit string", false);
  ADD_TEST_CASE(us5_tc04_bit_order_changes_which_stream_bits_are_pinned, "bit_order changes the pinned pattern", false);
  ADD_TEST_CASE(
    us5_tc05_byte_order_swaps_the_bytes_that_are_pinned, "little_endian pins what big_endian does byte-swapped", false
  );
}
