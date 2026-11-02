/* Story 11: composition, from `block NAME { ... };` to the flat fields the checks see.
 *
 * compose_file() is where named blocks stop existing: what comes out has to be exactly the packet
 * somebody could have written by hand, names and all, so these cases look at the fields it produces
 * and at checks 23 to 25. The bytes it leads to are story 10 of the system tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compose.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

typedef struct {
  DiagList diags;
  AstFile *file;
  bool composed;
} Composed;

static void compose_source(const char *source, Composed *out) {
  diag_init(&out->diags);
  out->file = parse(source, &out->diags);
  out->composed = out->diags.count == 0 && out->file != NULL && compose_file(out->file, &out->diags);
}

static void release(Composed *composed) {
  ast_free(composed->file);
  diag_free(&composed->diags);
}

/* Whether composing `source`, then checking it, reports check `wanted` (0 matches a message that
 * contains `text`, for the parse-level rules that carry no number). */
static bool reports(const char *source, int wanted, const char *text) {
  Composed composed;
  compose_source(source, &composed);
  if (composed.composed) {
    Layout *layout = check_file(composed.file, &composed.diags);
    layout_free(layout);
  }

  bool found = false;
  for (size_t i = 0; i < composed.diags.count; ++i) {
    if (composed.diags.items[i].check == wanted
        && (text == NULL || strstr(composed.diags.items[i].message, text) != NULL)) {
      found = true;
    }
  }
  release(&composed);
  return found;
}

static bool loads(const char *source) {
  Composed composed;
  compose_source(source, &composed);
  bool ok = composed.composed;
  if (ok) {
    Layout *layout = check_file(composed.file, &composed.diags);
    ok = layout != NULL;
    layout_free(layout);
  }
  release(&composed);
  return ok;
}

#define HEADER "block Header { id: 8; sequence: 16; version: 4 := 0x1; flags: 4; };\n"

static int us11_tc01_a_use_becomes_the_blocks_fields_under_its_name(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(HEADER "pkt A { hdr: Header { id := 0x02; }; tail: 8; };", &composed);

  const char *names[] = { "hdr.id", "hdr.sequence", "hdr.version", "hdr.flags", "tail" };
  int failure = 0;
  if (!composed.composed || composed.file->packets[0].field_count != 5) {
    failure = __LINE__;
  }
  for (size_t i = 0; i < 5 && failure == 0; ++i) {
    const AstField *field = &composed.file->packets[0].fields[i];
    if (strcmp(field->name, names[i]) != 0 || field->block != NULL) {
      failure = __LINE__;
    }
  }
  release(&composed);
  return failure;
}

/* The override fixes the field for this use; one the block fixed is replaced, not contradicted. */
static int us11_tc02_an_override_fixes_a_field_for_one_use(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(HEADER "pkt A { hdr: Header { id := 0x02; version := 0x2; }; };", &composed);

  int failure = 0;
  const AstField *fields = composed.composed ? composed.file->packets[0].fields : NULL;
  if (fields == NULL) {
    failure = __LINE__;
  } else if (!fields[0].has_fixed || fields[0].fixed != 0x02) {
    failure = __LINE__;
  } else if (fields[1].has_fixed) {
    failure = __LINE__;
  } else if (!fields[2].has_fixed || fields[2].fixed != 0x2) {
    failure = __LINE__;
  }
  /* The block itself is untouched: the next use sees its own fixed value, not this one's. */
  if (failure == 0 && composed.file->blocks[0].fields[2].fixed != 0x1) {
    failure = __LINE__;
  }
  release(&composed);
  return failure;
}

static int us11_tc03_the_same_block_twice_gives_two_sets_of_names(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source("block V { x: 16; y: 16; };\npkt A { id: 8 := 1; a: V; b: V; };", &composed);

  const char *names[] = { "id", "a.x", "a.y", "b.x", "b.y" };
  int failure = composed.composed && composed.file->packets[0].field_count == 5 ? 0 : __LINE__;
  for (size_t i = 0; i < 5 && failure == 0; ++i) {
    if (strcmp(composed.file->packets[0].fields[i].name, names[i]) != 0) {
      failure = __LINE__;
    }
  }
  release(&composed);
  return failure;
}

/* Nested blocks nest their names, and a nested field is overridden by its dotted path. */
static int us11_tc04_blocks_nest_and_a_path_reaches_inside(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(
    "block V { x: 16; z: 16; };\n"
    "block S { tag: 8; v: V; };\n"
    "pkt A { id: 8 := 1; s: S { v.z := 7; }; };",
    &composed
  );

  int failure = 0;
  if (!composed.composed || composed.file->packets[0].field_count != 4) {
    failure = __LINE__;
  } else {
    const AstField *z = &composed.file->packets[0].fields[3];
    if (strcmp(z->name, "s.v.z") != 0 || !z->has_fixed || z->fixed != 7) {
      failure = __LINE__;
    }
  }
  release(&composed);
  return failure;
}

/* A valof() inside a block reads the length of the same instance, not a field that shares its name. */
static int us11_tc05_a_valof_inside_a_block_names_the_same_instance(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(
    "block L { len: 8; data[4]: valof(len) * 8; };\n"
    "pkt A { id: 8 := 1; len: 8; a: L; b: L; };",
    &composed
  );

  int failure = 0;
  if (!composed.composed || composed.file->packets[0].field_count != 6) {
    failure = __LINE__;
  } else {
    const AstField *b_data = &composed.file->packets[0].fields[5];
    const Expr *width = b_data->width;
    if (strcmp(b_data->name, "b.data") != 0 || width->kind != EXPR_BINARY || width->lhs->kind != EXPR_VALOF
        || strcmp(width->lhs->reference, "b.len") != 0) {
      failure = __LINE__;
    }
  }
  release(&composed);
  if (failure == 0 && !loads("block L { len: 8; data[4]: valof(len) * 8; };\npkt A { id: 8 := 1; a: L; b: L; };")) {
    failure = __LINE__;
  }
  return failure;
}

static int us11_tc06_check_23_an_unknown_block(const TestArguments *args) {
  (void)args;
  return reports("pkt A { id: 8 := 1; h: Nope; };", 23, NULL) ? 0 : __LINE__;
}

static int us11_tc07_check_24_a_block_used_before_it_is_declared(const TestArguments *args) {
  (void)args;
  if (!reports("pkt A { id: 8 := 1; h: H; };\nblock H { x: 8; };", 24, NULL)) {
    return __LINE__;
  }
  /* Between blocks as well: the order is the file's, whatever uses what. */
  if (!reports("block T { u: U; };\nblock U { x: 8; };\npkt A { id: 8 := 1; t: T; };", 24, NULL)) {
    return __LINE__;
  }
  return 0;
}

/* Recursion is the same rule, and is caught whether or not a packet uses the block. */
static int us11_tc08_check_24_a_block_cannot_contain_itself(const TestArguments *args) {
  (void)args;
  if (!reports("block S { a: 8; s: S; };\npkt A { id: 8 := 1; };", 24, "uses itself")) {
    return __LINE__;
  }
  return 0;
}

static int us11_tc09_check_25_an_override_must_name_a_field_once(const TestArguments *args) {
  (void)args;
  if (!reports(HEADER "pkt A { hdr: Header { type := 1; }; };", 25, "no field 'type'")) {
    return __LINE__;
  }
  if (!reports(HEADER "pkt A { hdr: Header { id := 1; id := 2; }; };", 25, "twice")) {
    return __LINE__;
  }
  return 0;
}

/* The checks still apply to what composition produced: an override too wide for its field is check
 * 8, and two packets whose overrides do not tell them apart are check 12. */
static int us11_tc10_the_existing_checks_see_the_expanded_fields(const TestArguments *args) {
  (void)args;
  if (!reports(HEADER "pkt A { hdr: Header { id := 256; }; };", 8, NULL)) {
    return __LINE__;
  }
  if (!reports(HEADER "pkt A { hdr: Header { id := 1; }; };\npkt B { hdr: Header { id := 1; }; };", 12, NULL)) {
    return __LINE__;
  }
  if (!loads(HEADER "pkt A { hdr: Header { id := 1; }; };\npkt B { hdr: Header { id := 2; }; };")) {
    return __LINE__;
  }
  return 0;
}

/* An instance named like another field is a duplicate, though `hdr` and `hdr.id` no longer collide. */
static int us11_tc11_check_2_an_instance_shares_a_name(const TestArguments *args) {
  (void)args;
  if (!reports(HEADER "pkt A { hdr: 8 := 1; hdr: Header; };", 2, NULL)) {
    return __LINE__;
  }
  if (!reports(HEADER "block Header { x: 8; };\npkt A { id: 8 := 1; };", 1, "already a block")) {
    return __LINE__;
  }
  return 0;
}

/* Everything that would change a block's layout from the use site is refused where it is written. */
static int us11_tc12_a_use_cannot_reshape_its_block(const TestArguments *args) {
  (void)args;
  const struct {
    const char *source;
    const char *text;
  } cases[] = {
    { HEADER "pkt A { [ @byte_order: little_endian; ] hdr: Header { id := 1; }; };",
     "fixed where the block is declared"                                                                         },
    { HEADER "pkt A { hdr[4]: Header { id := 1; }; };",                              "no reservation of its own" },
    { HEADER "pkt A { hdr: Header := 3; };",                                         "no single value to fix"    },
    { HEADER "pkt A { id: 8 := 1; block Header { x: 8; }; };",                       "declared at file level"    },
    { "block E { };\npkt A { id: 8 := 1; };",                                        "at least one field"        },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    if (!reports(cases[i].source, 0, cases[i].text)) {
      return __LINE__;
    }
  }
  return 0;
}

/* ---- The import syntax ----------------------------------------------------------------------------
 *
 * What an import does is file-system work and is system story 11; what it looks like is here. */

static int us11_tc13_imports_parse_at_the_top_with_or_without_a_name(const TestArguments *args) {
  (void)args;
  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse(
    "import \"common/link.fdsl\";\n"
    "import \"vendor/s.fdsl\" as vendor;\n"
    "@byte_order: big_endian;\n"
    "pkt A { id: 8 := 1; h: vendor.Header { id := 2; }; [ @cal: vendor.celsius; ] t: 16; };",
    &diags
  );

  int failure = 0;
  const AstImport *first = file != NULL ? file->imports : NULL;
  const AstImport *second = first != NULL ? first->next : NULL;
  if (diags.count != 0 || first == NULL || second == NULL || second->next != NULL) {
    failure = __LINE__;
  } else if (strcmp(first->path, "common/link.fdsl") != 0 || first->alias != NULL
             || strcmp(second->path, "vendor/s.fdsl") != 0 || strcmp(second->alias, "vendor") != 0) {
    failure = __LINE__;
  } else if (strcmp(file->packets[0].fields[1].block, "vendor.Header") != 0
             || strcmp(file->packets[0].fields[2].cal, "vendor.celsius") != 0) {
    /* Dotted names are how what an `as` import brings is addressed. */
    failure = __LINE__;
  }
  ast_free(file);
  diag_free(&diags);
  return failure;
}

/* An import below anything else is refused where it is, rather than meaning "from here on". */
static int us11_tc14_an_import_goes_at_the_top(const TestArguments *args) {
  (void)args;
  if (!reports("pkt A { id: 8 := 1; };\nimport \"x.fdsl\";", 0, "at the top of the file")) {
    return __LINE__;
  }
  if (!reports("@bit_order: msb_first;\nimport \"x.fdsl\";\npkt A { id: 8 := 1; };", 0, "at the top of the file")) {
    return __LINE__;
  }
  return 0;
}

/* A path is the only text in the language: no escapes, and it closes on the line it opens. */
static int us11_tc15_a_path_is_a_string_on_one_line(const TestArguments *args) {
  (void)args;
  if (!reports("import \"x.fdsl\n\";\npkt A { id: 8 := 1; };", 0, "close on the line")) {
    return __LINE__;
  }
  if (!reports("import x;\npkt A { id: 8 := 1; };", 0, "quoted path")) {
    return __LINE__;
  }
  return 0;
}

/* Found by fuzzing (#59): the duplicate-name scan in compose.c's expand() is quadratic in a
 * container's declared field count, paid before the checker -- or COMPOSE_FIELD_MAX -- ever sees the
 * result. A count just over the ceiling proves the check fires without needing the tens of thousands
 * of block instances that made it slow before this. */
static int us11_tc16_check_29_a_block_or_packet_declares_too_many_fields(const TestArguments *args) {
  (void)args;

  size_t count = 4097;
  size_t capacity = strlen(HEADER) + 64 + count * 16;
  char *source = malloc(capacity);
  if (source == NULL) {
    return __LINE__;
  }
  size_t length = (size_t)snprintf(source, capacity, "%spkt A { id: 8 := 1;", HEADER);
  for (size_t i = 0; i < count; ++i) {
    length += (size_t)snprintf(source + length, capacity - length, " a%zu: Header;", i);
  }
  length += (size_t)snprintf(source + length, capacity - length, " };");

  bool found = reports(source, 29, "more fields");
  free(source);
  if (!found) {
    return __LINE__;
  }
  return 0;
}

void register_user_story11(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us11_tc01_a_use_becomes_the_blocks_fields_under_its_name, "a use becomes the block's fields under its name", true
  );
  ADD_TEST_CASE(us11_tc02_an_override_fixes_a_field_for_one_use, "an override fixes a field for one use only", false);
  ADD_TEST_CASE(
    us11_tc03_the_same_block_twice_gives_two_sets_of_names, "the same block twice gives two sets of names", false
  );
  ADD_TEST_CASE(
    us11_tc04_blocks_nest_and_a_path_reaches_inside, "blocks nest, and a dotted path overrides inside", false
  );
  ADD_TEST_CASE(
    us11_tc05_a_valof_inside_a_block_names_the_same_instance, "a valof() in a block reads its own instance", false
  );
  ADD_TEST_CASE(us11_tc06_check_23_an_unknown_block, "check 23: a use must name a block", false);
  ADD_TEST_CASE(
    us11_tc07_check_24_a_block_used_before_it_is_declared, "check 24: a block is declared before it is used", false
  );
  ADD_TEST_CASE(us11_tc08_check_24_a_block_cannot_contain_itself, "check 24: a block cannot contain itself", false);
  ADD_TEST_CASE(
    us11_tc09_check_25_an_override_must_name_a_field_once, "check 25: an override names a field, once", false
  );
  ADD_TEST_CASE(
    us11_tc10_the_existing_checks_see_the_expanded_fields, "checks 8 and 12 apply to the expanded fields", false
  );
  ADD_TEST_CASE(us11_tc11_check_2_an_instance_shares_a_name, "an instance or block name may not be reused", false);
  ADD_TEST_CASE(us11_tc12_a_use_cannot_reshape_its_block, "a use cannot change its block's layout", false);
  ADD_TEST_CASE(
    us11_tc13_imports_parse_at_the_top_with_or_without_a_name, "imports parse, flat or 'as', with dotted uses", false
  );
  ADD_TEST_CASE(us11_tc14_an_import_goes_at_the_top, "an import below anything else is refused", false);
  ADD_TEST_CASE(us11_tc15_a_path_is_a_string_on_one_line, "an import's path is a quoted string on one line", false);
  ADD_TEST_CASE(
    us11_tc16_check_29_a_block_or_packet_declares_too_many_fields,
    "check 29: a packet or block has at most 4096 declared fields",
    false
  );
}
