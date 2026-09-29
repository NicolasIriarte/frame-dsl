/* Story 7: attributes, the groups that attach them, and blocks.
 *
 * Two things are under test and they are easy to conflate. One is *resolution* -- which order a field
 * ends up with, given what four scopes said. The other is *extent* -- how far a group reaches, which
 * is the part a reader of a definition file has to be able to predict without running it. */
#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

typedef struct {
  DiagList diags;
  AstFile *file;
} Parsed;

static Parsed parse_source(const char *source) {
  Parsed parsed;
  diag_init(&parsed.diags);
  parsed.file = parse(source, &parsed.diags);
  return parsed;
}

static void parsed_free(Parsed *parsed) {
  ast_free(parsed->file);
  diag_free(&parsed->diags);
}

/* Reports whether parsing failed with a message containing `needle`. Matching the text rather than
 * merely counting errors is the point: these messages exist to say where an attribute may go, and a
 * message that no longer says so is a regression even while the file is still rejected. */
static bool rejected_saying(const char *source, const char *needle) {
  Parsed parsed = parse_source(source);
  bool found = false;
  for (size_t i = 0; i < parsed.diags.count; ++i) {
    if (strstr(parsed.diags.items[i].message, needle) != NULL) {
      found = true;
    }
  }
  parsed_free(&parsed);
  return found;
}

static int us7_tc01_a_file_attribute_reaches_every_packet(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "@bit_order: lsb_first;\n"
    "pkt A { h: 8 = 1; };\n"
    "pkt B { h: 8 = 2; };\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL || parsed.file->packet_count != 2) {
    failure = __LINE__;
  } else if (parsed.file->packets[0].fields[0].bit_order != BIT_ORDER_LSB_FIRST
             || parsed.file->packets[1].fields[0].bit_order != BIT_ORDER_LSB_FIRST) {
    failure = __LINE__;
  } else if (parsed.file->packets[0].fields[0].bit_order_written) {
    failure = __LINE__; /* inherited, not written here: check 10 turns on the difference */
  }

  parsed_free(&parsed);
  return failure;
}

/* A group in front of `pkt` covers that packet and stops at it. */
static int us7_tc02_a_packet_group_stops_at_the_packet(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "[ @bit_order: lsb_first; ]\n"
    "pkt A { h: 8 = 1; };\n"
    "pkt B { h: 8 = 2; };\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL || parsed.file->packet_count != 2) {
    failure = __LINE__;
  } else if (parsed.file->packets[0].fields[0].bit_order != BIT_ORDER_LSB_FIRST) {
    failure = __LINE__;
  } else if (parsed.file->packets[1].fields[0].bit_order != BIT_ORDER_MSB_FIRST) {
    failure = __LINE__; /* the next packet is back to the default */
  }

  parsed_free(&parsed);
  return failure;
}

/* The case the whole design turns on: a group before a field reaches exactly one field. */
static int us7_tc03_a_field_group_reaches_one_field(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "pkt A {\n"
    "  h: 8 = 1;\n"
    "  [ @bit_order: lsb_first; ] a: 8;\n"
    "  b: 8;\n"
    "};\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL) {
    failure = __LINE__;
  } else {
    const AstField *fields = parsed.file->packets[0].fields;
    if (fields[1].bit_order != BIT_ORDER_LSB_FIRST || !fields[1].bit_order_written) {
      failure = __LINE__;
    } else if (fields[2].bit_order != BIT_ORDER_MSB_FIRST) {
      failure = __LINE__; /* and not the one after it */
    }
  }

  parsed_free(&parsed);
  return failure;
}

static int us7_tc04_a_block_group_reaches_every_field_in_it(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "pkt A {\n"
    "  h: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  block { a: 16; b: 16; };\n"
    "  c: 16;\n"
    "};\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL) {
    failure = __LINE__;
  } else {
    const AstField *fields = parsed.file->packets[0].fields;
    if (fields[1].byte_order != BYTE_ORDER_LITTLE_ENDIAN || fields[2].byte_order != BYTE_ORDER_LITTLE_ENDIAN) {
      failure = __LINE__;
    } else if (fields[3].byte_order != BYTE_ORDER_BIG_ENDIAN) {
      failure = __LINE__; /* and nothing after the block */
    }
  }

  parsed_free(&parsed);
  return failure;
}

/* A block's fields are appended to the packet with consecutive offsets and their own flat names. The
 * AST never nests, which is why nothing downstream of the parser knows blocks exist. */
static int us7_tc05_a_block_is_invisible_to_the_layout(const TestArguments *args) {
  (void)args;

  Parsed with = parse_source("pkt A { h: 8 = 1; block { a: 8; b: 8; }; c: 8; };");
  Parsed without = parse_source("pkt A { h: 8 = 1; a: 8; b: 8; c: 8; };");

  int failure = 0;
  if (with.file == NULL || without.file == NULL || with.diags.count != 0) {
    failure = __LINE__;
  } else if (with.file->packets[0].field_count != without.file->packets[0].field_count) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < with.file->packets[0].field_count; ++i) {
      if (strcmp(with.file->packets[0].fields[i].name, without.file->packets[0].fields[i].name) != 0) {
        failure = __LINE__;
        break;
      }
    }
  }

  parsed_free(&with);
  parsed_free(&without);
  return failure;
}

static int us7_tc06_blocks_nest_and_the_innermost_wins(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "pkt A {\n"
    "  h: 8 = 1;\n"
    "  [ @byte_order: little_endian; ]\n"
    "  block {\n"
    "    a: 16;\n"
    "    [ @byte_order: big_endian; ]\n"
    "    block { b: 16; };\n"
    "    c: 16;\n"
    "  };\n"
    "};\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL) {
    failure = __LINE__;
  } else {
    const AstField *fields = parsed.file->packets[0].fields;
    if (fields[1].byte_order != BYTE_ORDER_LITTLE_ENDIAN) {
      failure = __LINE__;
    } else if (fields[2].byte_order != BYTE_ORDER_BIG_ENDIAN) {
      failure = __LINE__; /* the inner block */
    } else if (fields[3].byte_order != BYTE_ORDER_LITTLE_ENDIAN) {
      failure = __LINE__; /* and back out of it again */
    }
  }

  parsed_free(&parsed);
  return failure;
}

/* Innermost wins across all four scopes at once. */
static int us7_tc07_resolution_runs_field_block_packet_file(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source(
    "@byte_order: big_endian;\n"
    "[ @byte_order: little_endian; ]\n"
    "pkt A {\n"
    "  h: 8 = 1;\n"
    "  [ @byte_order: big_endian; ]\n"
    "  block {\n"
    "    a: 16;\n"
    "    [ @byte_order: little_endian; ] b: 16;\n"
    "  };\n"
    "};\n"
  );

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL) {
    failure = __LINE__;
  } else {
    const AstField *fields = parsed.file->packets[0].fields;
    if (fields[0].byte_order != BYTE_ORDER_LITTLE_ENDIAN) {
      failure = __LINE__; /* packet beats file */
    } else if (fields[1].byte_order != BYTE_ORDER_BIG_ENDIAN || fields[1].byte_order_written) {
      failure = __LINE__; /* block beats packet, and is inherited by the field */
    } else if (fields[2].byte_order != BYTE_ORDER_LITTLE_ENDIAN || !fields[2].byte_order_written) {
      failure = __LINE__; /* field beats block, and is written */
    }
  }

  parsed_free(&parsed);
  return failure;
}

static int us7_tc08_a_bare_attribute_inside_a_packet_is_rejected(const TestArguments *args) {
  (void)args;
  const char *source = "pkt A { h: 8 = 1; @bit_order: lsb_first; a: 8; };";
  if (!rejected_saying(source, "[ ... ]") || !rejected_saying(source, "goes at the top of the file")
      || rejected_saying(source, "first packet")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The defaults go before every declaration, not only before the first packet (#122): after a packet, a
 * calibration or a block, the message names the first declaration and where it starts -- at its group,
 * when it has one, since the defaults go above that too. */
static int us7_tc09_a_bare_attribute_after_a_declaration_is_rejected(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    "pkt A { h: 8 = 1; };\n@bit_order: lsb_first;\npkt B { h: 8 = 2; };",
    "cal c(x) = x;\n@bit_order: lsb_first;\npkt A { h: 8 = 1; [ @cal: c; ] v: 8; };",
    "[ @byte_order: little_endian; ]\nblock B { v: 8; };\n@bit_order: lsb_first;\npkt A { h: 8 = 1; B; };",
  };
  const char *expected[] = {
    "before the first declaration (the 'pkt' starting on line 1)",
    "before the first declaration (the 'cal' starting on line 1)",
    "before the first declaration (the 'block' starting on line 1)",
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    if (!rejected_saying(sources[i], expected[i]) || !rejected_saying(sources[i], "for one packet or block")
        || rejected_saying(sources[i], "first packet")) {
      return __LINE__ * 10 + (int)i;
    }
  }

  /* After a group, the attribute belongs in it; after a misplaced import, there is no declaration to
   * name. And written where they belong -- after the imports, before everything -- defaults are fine. */
  if (!rejected_saying(
        "[ @byte_order: little_endian; ]\n@bit_order: lsb_first;\npkt A { h: 8 = 1; };", "put this one inside the group"
      )) {
    return __LINE__;
  }
  if (!rejected_saying(
        "@byte_order: little_endian;\nimport \"x.fdsl\";\n@bit_order: lsb_first;\npkt A { h: 8 = 1; };",
        "after the imports and before the first declaration"
      )) {
    return __LINE__;
  }
  Parsed parsed = parse_source(
    "@byte_order: little_endian;\n@bit_order: lsb_first;\ncal c(x) = x;\nblock B { v: 8; };\n"
    "pkt A { h: 8 = 1; B; [ @cal: c; ] w: 8; };"
  );
  int failure = parsed.diags.count == 0 ? 0 : __LINE__;
  parsed_free(&parsed);
  return failure;
}

/* Reported by name, rather than as a parse failure two tokens further on. */
static int us7_tc10_an_unknown_attribute_is_named(const TestArguments *args) {
  (void)args;
  if (!rejected_saying("@endianness: big;\npkt A { h: 8 = 1; };", "unknown attribute 'endianness'")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us7_tc11_a_bad_value_is_reported_with_what_was_written(const TestArguments *args) {
  (void)args;
  if (!rejected_saying("@bit_order: sideways;\npkt A { h: 8 = 1; };", "'sideways'")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us7_tc12_empty_groups_and_blocks_are_rejected(const TestArguments *args) {
  (void)args;
  if (!rejected_saying("pkt A { h: 8 = 1; [ ] a: 8; };", "at least one attribute")) {
    FORCE_FAILURE();
  }
  if (!rejected_saying("pkt A { h: 8 = 1; block { }; };", "at least one field")) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The reason the attribute names left the lexer. Six words came back into circulation; this pins one
 * of them so nobody quietly reserves it again. */
static int us7_tc13_attribute_names_are_usable_as_field_names(const TestArguments *args) {
  (void)args;

  Parsed parsed = parse_source("pkt A { h: 8 = 1; bit_order: 4; little_endian: 4; };");

  int failure = 0;
  if (parsed.diags.count != 0 || parsed.file == NULL) {
    failure = __LINE__;
  } else if (strcmp(parsed.file->packets[0].fields[1].name, "bit_order") != 0
             || strcmp(parsed.file->packets[0].fields[2].name, "little_endian") != 0) {
    failure = __LINE__;
  }

  parsed_free(&parsed);
  return failure;
}

void register_user_story7(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us7_tc01_a_file_attribute_reaches_every_packet, "a file attribute applies to every packet", false);
  ADD_TEST_CASE(us7_tc02_a_packet_group_stops_at_the_packet, "a group before 'pkt' covers that packet only", false);
  ADD_TEST_CASE(us7_tc03_a_field_group_reaches_one_field, "a group before a field covers one field", false);
  ADD_TEST_CASE(
    us7_tc04_a_block_group_reaches_every_field_in_it, "a group before a block covers the block and stops there", false
  );
  ADD_TEST_CASE(
    us7_tc05_a_block_is_invisible_to_the_layout, "a block produces the same fields as writing them out", false
  );
  ADD_TEST_CASE(us7_tc06_blocks_nest_and_the_innermost_wins, "blocks nest", false);
  ADD_TEST_CASE(us7_tc07_resolution_runs_field_block_packet_file, "resolution is field > block > packet > file", false);
  ADD_TEST_CASE(
    us7_tc08_a_bare_attribute_inside_a_packet_is_rejected,
    "a bare attribute inside a packet says where it belongs",
    false
  );
  ADD_TEST_CASE(
    us7_tc09_a_bare_attribute_after_a_declaration_is_rejected, "a bare attribute after a declaration names it", false
  );
  ADD_TEST_CASE(us7_tc10_an_unknown_attribute_is_named, "an unknown attribute is named", false);
  ADD_TEST_CASE(us7_tc11_a_bad_value_is_reported_with_what_was_written, "a bad attribute value is quoted back", false);
  ADD_TEST_CASE(us7_tc12_empty_groups_and_blocks_are_rejected, "an empty group and an empty block are rejected", false);
  ADD_TEST_CASE(us7_tc13_attribute_names_are_usable_as_field_names, "'bit_order' is an ordinary field name", false);
}
