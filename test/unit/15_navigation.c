/* Story 15: hover, go to definition and completion (#72), asked of the navigator directly.
 *
 * Positions are the compiler's -- lines and byte columns from 1 -- which is what lsp.c hands the
 * navigator after converting the editor's. Files that import are written into the case's own directory
 * first, so an import is resolved the way the server resolves it: beside the file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "lsp_navigate.h"
#include "parser.h"
#include "testsuite.h"

static bool write_file(const TestArguments *args, const char *name, const char *content, char *path, size_t capacity) {
  snprintf(path, capacity, "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static bool contains(const char *text, const char *needle) {
  return text != NULL && strstr(text, needle) != NULL;
}

/* Whether completing at (line, column) offers exactly `want`, a NULL-terminated list, in any order. */
static bool completes_to(const Navigator *navigator, int line, int column, const char *const *want) {
  size_t count = 0;
  NavCompletion *items = nav_complete(navigator, line, column, &count);
  size_t wanted = 0;
  while (want[wanted] != NULL) {
    wanted += 1;
  }
  bool same = items != NULL && count == wanted;
  for (size_t i = 0; same && i < wanted; ++i) {
    bool found = false;
    for (size_t j = 0; j < count; ++j) {
      found |= strcmp(items[j].label, want[i]) == 0;
    }
    same = found;
  }
  nav_completions_free(items, count);
  return same;
}

static const char *const GROUPED =
  "@byte_order: big_endian;\n"
  "pkt P {\n"
  "  id: 8 = 1;\n"
  "  [ @byte_order: little_endian; ]\n"
  "  v:  16;\n"
  "};\n";

static int us15_tc01_hover_shows_where_a_field_lands(const TestArguments *args) {
  (void)args;
  Navigator *navigator = nav_open(GROUPED, NULL);
  NavLocation range;
  char *text = navigator != NULL ? nav_hover(navigator, 5, 3, &range) : NULL;
  int failure = 0;
  if (!contains(text, "**v**") || !contains(text, "width: 16 bits") || !contains(text, "offset: bit 8 (byte 1)")) {
    failure = __LINE__;
  } else if (!contains(text, "byte order: little_endian")) {
    failure = __LINE__; /* the group's, not the file's */
  } else if (range.line != 5 || range.column != 3 || range.end_column != 4) {
    failure = __LINE__;
  }
  free(text);

  /* The file's own default, on the field the group does not cover. */
  text = failure == 0 ? nav_hover(navigator, 3, 3, &range) : NULL;
  if (failure == 0 && (!contains(text, "byte order: big_endian") || !contains(text, "fixed at 0x1"))) {
    failure = __LINE__;
  }
  free(text);
  nav_close(navigator);
  return failure;
}

static const char *const INSTANCES =
  "block H {\n"
  "  a:  8;\n"
  "  id: 8;\n"
  "};\n"
  "pkt P {\n"
  "  x:   8;\n"
  "  hdr: H { id = 2; };\n"
  "};\n";

static int us15_tc02_hover_inside_an_instance_shows_its_offset_in_the_packet(const TestArguments *args) {
  (void)args;
  Navigator *navigator = nav_open(INSTANCES, NULL);
  NavLocation range;
  char *field = navigator != NULL ? nav_hover(navigator, 7, 12, &range) : NULL; /* `id` in the override */
  char *instance = navigator != NULL ? nav_hover(navigator, 7, 3, &range) : NULL; /* `hdr` */
  char *declared = navigator != NULL ? nav_hover(navigator, 3, 3, &range) : NULL; /* `id` in the block */
  int failure = 0;
  if (!contains(field, "**hdr.id**") || !contains(field, "offset: bit 16 (byte 2)")
      || !contains(field, "fixed at 0x2")) {
    failure = __LINE__;
  } else if (!contains(instance, "instance of block `H`") || !contains(instance, "bits 8 to 23")) {
    failure = __LINE__;
  } else if (!contains(declared, "field of block `H`") || contains(declared, "offset: bit")) {
    failure = __LINE__; /* a block's field has no offset until a packet uses it */
  }
  free(field);
  free(instance);
  free(declared);
  nav_close(navigator);
  return failure;
}

static bool lands_on(
  const Navigator *navigator,
  int line,
  int column,
  const char *file,
  int want_line,
  int want_column
) {
  NavLocation where = { 0 };
  bool found = nav_definition(navigator, line, column, &where);
  bool same = found && where.line == want_line && where.column == want_column
              && (file == NULL ? where.file == NULL : where.file != NULL && strstr(where.file, file) != NULL);
  free(where.file);
  return same;
}

static int us15_tc03_definition_follows_every_kind_of_name(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "link.fdsl", "block Header {\n  id: 8;\n};\ncal volts(raw) = raw * 2;\n", path, sizeof(path))) {
    FORCE_FAILURE();
  }
  const char *source =
    "import \"link.fdsl\";\n"
    "import \"link.fdsl\" as link;\n"
    "cal f(x) = x + 1;\n"
    "block B {\n"
    "  b: 8;\n"
    "};\n"
    "pkt P {\n"
    "  hdr:  Header { id = 1; };\n"
    "  ext:  link.Header;\n"
    "  in:   B;\n"
    "  len:  8;\n"
    "  [ @cal: f; ]\n"
    "  v:    16;\n"
    "  [ @cal: link.volts; ]\n"
    "  w:    16;\n"
    "  d[4]: valof(len) * 8;\n"
    "};\n";
  if (!write_file(args, "device.fdsl", source, path, sizeof(path))) {
    FORCE_FAILURE();
  }
  Navigator *navigator = nav_open(source, path);
  int failure = 0;
  if (navigator == NULL) {
    failure = __LINE__;
  } else if (!lands_on(navigator, 16, 16, NULL, 11, 3)) {
    failure = __LINE__; /* valof(len) */
  } else if (!lands_on(navigator, 10, 9, NULL, 4, 7)) {
    failure = __LINE__; /* a local block */
  } else if (!lands_on(navigator, 12, 11, NULL, 3, 5)) {
    failure = __LINE__; /* a local calibration */
  } else if (!lands_on(navigator, 8, 10, "link.fdsl", 1, 7)) {
    failure = __LINE__; /* a block through a flat import, in the other file */
  } else if (!lands_on(navigator, 9, 14, "link.fdsl", 1, 7)) {
    failure = __LINE__; /* the same through `as` */
  } else if (!lands_on(navigator, 14, 16, "link.fdsl", 4, 5)) {
    failure = __LINE__; /* a calibration through `as` */
  } else if (!lands_on(navigator, 9, 9, NULL, 2, 1)) {
    failure = __LINE__; /* the namespace itself: its import */
  } else if (!lands_on(navigator, 8, 18, "link.fdsl", 2, 3)) {
    failure = __LINE__; /* an override, onto the imported block's field */
  }
  nav_close(navigator);
  return failure;
}

static int us15_tc04_attribute_completion_is_what_the_parser_knows(const TestArguments *args) {
  (void)args;
  const char *source = "pkt P {\n  [ @\n";
  Navigator *navigator = nav_open(source, NULL);
  int failure = navigator == NULL || !completes_to(navigator, 2, 6, NAV_ATTRIBUTE_NAMES) ? __LINE__ : 0;
  nav_close(navigator);

  /* Each is one the parser accepts as a name: whatever it says about the value, it is not "unknown". */
  for (size_t i = 0; failure == 0 && NAV_ATTRIBUTE_NAMES[i] != NULL; ++i) {
    char text[128];
    snprintf(text, sizeof(text), "pkt P { [ @%s: msb_first; ] x: 8 = 1; };\n", NAV_ATTRIBUTE_NAMES[i]);
    DiagList diags;
    diag_init(&diags);
    AstFile *file = parse(text, &diags);
    for (size_t d = 0; d < diags.count; ++d) {
      failure = strstr(diags.items[d].message, "unknown attribute") != NULL ? __LINE__ : failure;
    }
    ast_free(file);
    diag_free(&diags);
  }

  static const char *const BYTE_ORDERS[] = { "big_endian", "little_endian", NULL };
  navigator = failure == 0 ? nav_open("pkt P {\n  [ @byte_order: \n", NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 2, 17, BYTE_ORDERS))) {
    failure = __LINE__;
  }
  nav_close(navigator);
  return failure;
}

static int us15_tc05_valof_completion_offers_scalars_declared_above(const TestArguments *args) {
  (void)args;
  const char *source =
    "pkt P {\n"
    "  a:    8 = 1;\n"
    "  wide: 128;\n"
    "  n:    4 * 4;\n"
    "  d[4]: valof(\n"
    "  later: 8;\n"
    "};\n";
  static const char *const WANT[] = { "a", "n", NULL };
  static const char *const PREFIXED[] = { "n", NULL };
  Navigator *navigator = nav_open(source, NULL);
  int failure = navigator == NULL || !completes_to(navigator, 5, 15, WANT) ? __LINE__ : 0;
  nav_close(navigator);

  navigator = failure == 0 ? nav_open("pkt P {\n  a: 8 = 1;\n  n: 4;\n  d[4]: valof(n", NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 4, 16, PREFIXED))) {
    failure = __LINE__;
  }
  nav_close(navigator);
  return failure;
}

static int us15_tc06_a_namespace_offers_what_its_import_brings(const TestArguments *args) {
  char path[1024];
  if (!write_file(
        args,
        "lib.fdsl",
        "block Header { id: 8; };\nblock Tail { crc: 16; };\ncal volts(raw) = raw;\n",
        path,
        sizeof(path)
      )) {
    FORCE_FAILURE();
  }
  const char *source = "import \"lib.fdsl\" as lib;\npkt P {\n  h: lib.\n  [ @cal: lib.\n";
  if (!write_file(args, "device.fdsl", source, path, sizeof(path))) {
    FORCE_FAILURE();
  }
  static const char *const BLOCKS[] = { "Header", "Tail", NULL };
  static const char *const CALS[] = { "volts", NULL };
  Navigator *navigator = nav_open(source, path);
  int failure = 0;
  if (navigator == NULL || !completes_to(navigator, 3, 10, BLOCKS)) {
    failure = __LINE__;
  } else if (!completes_to(navigator, 4, 15, CALS)) {
    failure = __LINE__;
  }
  nav_close(navigator);
  return failure;
}

/* Every question at every position of every prefix of a file being typed, and of text that is not a
 * definition at all: answered, or answered with nothing, and never a crash. */
static int us15_tc07_a_broken_file_is_answered_with_what_can_be_said(const TestArguments *args) {
  (void)args;
  const char *typed =
    "import \"missing.fdsl\" as n;\n"
    "cal f(x) = x * 2;\n"
    "block H {\n  id: 8 = 1;\n  inner: n.X { a.b = 1; };\n};\n"
    "pkt TM {\n  h: H { id = 2; };\n  [ @cal: f; ]\n  v[2]: valof(v) * 8;\n  H;\n};\n";
  size_t length = strlen(typed);
  char buffer[512];
  for (size_t prefix = 0; prefix <= length; prefix += 3) {
    snprintf(buffer, sizeof(buffer), "%.*s", (int)prefix, typed);
    Navigator *navigator = nav_open(buffer, NULL);
    if (navigator == NULL) {
      return __LINE__;
    }
    for (int line = 1; line <= 14; ++line) {
      for (int column = 1; column <= 30; ++column) {
        NavLocation where = { 0 };
        free(nav_hover(navigator, line, column, &where));
        if (nav_definition(navigator, line, column, &where)) {
          free(where.file);
        }
        size_t count = 0;
        NavCompletion *items = nav_complete(navigator, line, column, &count);
        nav_completions_free(items, count);
      }
    }
    nav_close(navigator);
  }

  /* Not a definition at all: nothing to say about any of it. */
  Navigator *navigator = nav_open("}}} ;; [[ @@ :: ( valof ) . .\n\"unterminated", NULL);
  int failure = navigator == NULL ? __LINE__ : 0;
  for (int column = 1; failure == 0 && column <= 32; ++column) {
    NavLocation where = { 0 };
    char *text = nav_hover(navigator, 1, column, &where);
    if (text != NULL || nav_definition(navigator, 1, column, &where)) {
      failure = __LINE__;
    }
    free(text);
  }
  nav_close(navigator);
  return failure;
}

void register_user_story15(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us15_tc01_hover_shows_where_a_field_lands, "hover shows width, offset and the orders in force", true);
  ADD_TEST_CASE(
    us15_tc02_hover_inside_an_instance_shows_its_offset_in_the_packet,
    "hover inside an instance shows its offset in the packet",
    false
  );
  ADD_TEST_CASE(us15_tc03_definition_follows_every_kind_of_name, "definition follows every kind of name", false);
  ADD_TEST_CASE(
    us15_tc04_attribute_completion_is_what_the_parser_knows, "attribute completion is what the parser knows", false
  );
  ADD_TEST_CASE(
    us15_tc05_valof_completion_offers_scalars_declared_above, "valof( offers the scalars declared above", false
  );
  ADD_TEST_CASE(us15_tc06_a_namespace_offers_what_its_import_brings, "ns. offers what that import brings", false);
  ADD_TEST_CASE(
    us15_tc07_a_broken_file_is_answered_with_what_can_be_said, "a broken file is answered, never crashed on", false
  );
}
