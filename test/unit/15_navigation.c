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
#include "lsp_semantic.h"
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
  "  id: 8 := 1;\n"
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
  "  hdr: H { id := 2; };\n"
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
    "  hdr:  Header { id := 1; };\n"
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
    snprintf(text, sizeof(text), "pkt P { [ @%s: msb_first; ] x: 8 := 1; };\n", NAV_ATTRIBUTE_NAMES[i]);
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
    "  a:    8 := 1;\n"
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

  navigator = failure == 0 ? nav_open("pkt P {\n  a: 8 := 1;\n  n: 4;\n  d[4]: valof(n", NULL) : NULL;
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
    "block H {\n  id: 8 := 1;\n  inner: n.X { a.b := 1; };\n};\n"
    "enum E: 2 {\n  A;\n  B = 2;\n};\n"
    "pkt TM {\n  h: H { id := 2; };\n  e: E := B;\n  [ @cal: f; ]\n  v[2]: valof(v) * 8;\n  H;\n};\n";
  size_t length = strlen(typed);
  char buffer[1024];
  for (size_t prefix = 0; prefix <= length; prefix += 3) {
    snprintf(buffer, sizeof(buffer), "%.*s", (int)prefix, typed);
    Navigator *navigator = nav_open(buffer, NULL);
    if (navigator == NULL) {
      return __LINE__;
    }
    for (int line = 1; line <= 19; ++line) {
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

/* ---- Enumerations (#11) ------------------------------------------------------------------------ */

static const char *const RADIO =
  "import \"modes.fdsl\" as vendor;\n" /* 1 */
  "enum PktType: 4 {\n" /* 2 */
  "  HK = 0x1;\n" /* 3 */
  "  EVENT;\n" /* 4 */
  "};\n" /* 5 */
  "block Header {\n" /* 6 */
  "  type: PktType;\n" /* 7 */
  "  seq:  12;\n" /* 8 */
  "};\n" /* 9 */
  "pkt TM {\n" /* 10 */
  "  hdr:  Header { type := EVENT; };\n" /* 11 */
  "  kind: PktType := HK;\n" /* 12 */
  "  mode: vendor.Mode := TX;\n" /* 13 */
  "  pad:  6;\n" /* 14 */
  "};\n";

static int us15_tc08_an_enum_and_its_names_are_navigable(const TestArguments *args) {
  char path[1024];
  if (!write_file(args, "modes.fdsl", "enum Mode: 2 {\n  RX = 1;\n  TX;\n};\n", path, sizeof(path))
      || !write_file(args, "radio.fdsl", RADIO, path, sizeof(path))) {
    FORCE_FAILURE();
  }
  Navigator *navigator = nav_open(RADIO, path);
  NavLocation range;
  char *type = navigator != NULL ? nav_hover(navigator, 12, 9, &range) : NULL; /* `PktType` */
  char *named = navigator != NULL ? nav_hover(navigator, 12, 20, &range) : NULL; /* `HK` */
  char *override = navigator != NULL ? nav_hover(navigator, 11, 26, &range) : NULL; /* `EVENT` */
  char *imported = navigator != NULL ? nav_hover(navigator, 13, 16, &range) : NULL; /* `Mode` */
  char *attribute = navigator != NULL ? nav_hover(navigator, 1, 1, &range) : NULL;
  int failure = 0;
  if (!contains(type, "**enum PktType** -- 4 bits") || !contains(type, "EVENT = 2;")) {
    failure = __LINE__;
  } else if (!contains(named, "**HK** = 1") || !contains(named, "enum `PktType`")) {
    failure = __LINE__;
  } else if (!contains(override, "**EVENT** = 2")) {
    failure = __LINE__; /* looked up in the enum of the block's field */
  } else if (!contains(imported, "**enum vendor.Mode**") || !contains(imported, "modes.fdsl")) {
    failure = __LINE__;
  } else if (!lands_on(navigator, 12, 9, NULL, 2, 6)) {
    failure = __LINE__; /* a field's enum, to its declaration */
  } else if (!lands_on(navigator, 12, 20, NULL, 3, 3)) {
    failure = __LINE__; /* a value, to its name in the enum */
  } else if (!lands_on(navigator, 11, 26, NULL, 4, 3)) {
    failure = __LINE__; /* an override's value, through the block's field */
  } else if (!lands_on(navigator, 13, 24, "modes.fdsl", 3, 3)) {
    failure = __LINE__; /* an imported enum's name, in its own file */
  }
  free(type);
  free(named);
  free(override);
  free(imported);
  free(attribute);
  nav_close(navigator);
  return failure;
}

static int us15_tc09_completion_offers_enums_and_their_names(const TestArguments *args) {
  (void)args;
  const char *types = "enum PowerState: 1 {\n  OFF;\n  ON;\n};\nblock Header {\n  id: 8;\n};\npkt P {\n  p: \n";
  static const char *const TYPES[] = { "PowerState", "Header", "valof", NULL };
  Navigator *navigator = nav_open(types, NULL);
  int failure = navigator == NULL || !completes_to(navigator, 9, 6, TYPES) ? __LINE__ : 0;
  nav_close(navigator);

  static const char *const NAMES[] = { "OFF", "ON", NULL };
  const char *value = "enum PowerState: 1 {\n  OFF;\n  ON;\n};\npkt P {\n  p: PowerState := \n";
  navigator = failure == 0 ? nav_open(value, NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 6, 19, NAMES))) {
    failure = __LINE__;
  }
  nav_close(navigator);

  const char *override =
    "enum PowerState: 1 {\n  OFF;\n  ON;\n};\nblock B {\n  p: PowerState;\n};\npkt P {\n  b: B { p := \n";
  navigator = failure == 0 ? nav_open(override, NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 9, 15, NAMES))) {
    failure = __LINE__;
  }
  nav_close(navigator);

  static const char *const TOP[] = { "pkt", "block", "cal", "enum", "import", NULL };
  navigator = failure == 0 ? nav_open("", NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 1, 1, TOP))) {
    failure = __LINE__;
  }
  nav_close(navigator);
  return failure;
}

/* What each word of an enum and its uses is, before anything is compiled. */
static int us15_tc10_semantic_tokens_know_an_enum_from_a_block(const TestArguments *args) {
  (void)args;
  size_t count = 0;
  SemanticToken *tokens = semantic_tokens(RADIO, &count);
  const struct {
    int line;
    int column;
    SemanticType type;
    bool declaration;
  } want[] = {
    { 2,  1,  SEM_KEYWORD,     false }, /* enum */
    { 2,  6,  SEM_ENUM,        true  }, /* PktType */
    { 3,  3,  SEM_ENUM_MEMBER, true  }, /* HK */
    { 4,  3,  SEM_ENUM_MEMBER, true  }, /* EVENT */
    { 7,  9,  SEM_ENUM,        false }, /* type: PktType */
    { 11, 9,  SEM_STRUCT,      false }, /* Header, a block */
    { 11, 18, SEM_PROPERTY,    false }, /* type := */
    { 11, 26, SEM_ENUM_MEMBER, false }, /* := EVENT */
    { 12, 9,  SEM_ENUM,        false }, /* kind: PktType */
    { 12, 20, SEM_ENUM_MEMBER, false }, /* := HK */
    { 13, 24, SEM_ENUM_MEMBER, false }, /* := TX */
  };
  int failure = tokens == NULL ? __LINE__ : 0;
  for (size_t w = 0; failure == 0 && w < sizeof(want) / sizeof(*want); ++w) {
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      if (tokens[i].line == want[w].line && tokens[i].column == want[w].column) {
        found = tokens[i].type == want[w].type && tokens[i].declaration == want[w].declaration;
      }
    }
    failure = found ? 0 : __LINE__ * 100 + (int)w;
  }
  free(tokens);
  return failure;
}

/* `[ @if: valof(hdr.flag) == 1; ]` (#12): each part of the name inside goes to what it names, an enum's
 * name after `==` or `!=` to its declaration, and a conditional field says what it is present under. */
static const char *const CONDITIONAL =
  "enum Kind: 4 { IDLE; RUN; };\n"
  "block H { flag: 8; kind: Kind; pad: 4; };\n"
  "pkt P {\n"
  "  id:  8 := 1;\n"
  "  hdr: H;\n"
  "  [ @if: valof(hdr.flag) == 1; ] e: 8;\n"
  "  [ @if: valof(hdr.kind) != RUN; ] f: 8;\n"
  "};\n";

static int us15_tc11_a_condition_is_navigable(const TestArguments *args) {
  (void)args;
  Navigator *navigator = nav_open(CONDITIONAL, NULL);
  NavLocation at = { 0 };
  int failure = navigator == NULL ? __LINE__ : 0;

  if (failure == 0 && (!nav_definition(navigator, 6, 16, &at) || at.line != 5 || at.column != 3)) {
    failure = __LINE__; /* hdr, onto the instance */
  }
  free(at.file);
  at.file = NULL;
  if (failure == 0 && (!nav_definition(navigator, 6, 20, &at) || at.line != 2 || at.column != 11)) {
    failure = __LINE__; /* flag, onto the block's field */
  }
  free(at.file);
  at.file = NULL;
  if (failure == 0 && (!nav_definition(navigator, 7, 29, &at) || at.line != 1 || at.column != 22)) {
    failure = __LINE__; /* RUN, onto the enum's name */
  }
  free(at.file);

  NavLocation range;
  char *text = failure == 0 ? nav_hover(navigator, 6, 6, &range) : NULL;
  if (failure == 0 && !contains(text, "**@if**")) {
    failure = __LINE__;
  }
  free(text);
  text = failure == 0 ? nav_hover(navigator, 6, 34, &range) : NULL;
  if (failure == 0 && !contains(text, "present only if `valof(hdr.flag) == 1`")) {
    failure = __LINE__;
  }
  free(text);
  text = failure == 0 ? nav_hover(navigator, 7, 20, &range) : NULL;
  if (failure == 0 && !contains(text, "**hdr.kind**")) {
    failure = __LINE__;
  }
  free(text);
  nav_close(navigator);

  /* After `@if:` comes valof(); after `==` on an enum's field, its names. */
  static const char *const VALOF[] = { "valof", NULL };
  static const char *const NAMES[] = { "IDLE", "RUN", NULL };
  navigator = failure == 0 ? nav_open("pkt P {\n  id: 8 := 1;\n  [ @if: \n", NULL) : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 3, 10, VALOF))) {
    failure = __LINE__;
  }
  nav_close(navigator);
  navigator =
    failure == 0
      ? nav_open(
          "enum Kind: 4 { IDLE; RUN; };\npkt P {\n  id: 8 := 1;\n  k: Kind;\n  p: 4;\n  [ @if: valof(k) == \n", NULL
        )
      : NULL;
  if (failure == 0 && (navigator == NULL || !completes_to(navigator, 6, 22, NAMES))) {
    failure = __LINE__;
  }
  nav_close(navigator);
  return failure;
}

static int us15_tc12_semantic_tokens_read_a_condition(const TestArguments *args) {
  (void)args;
  size_t count = 0;
  SemanticToken *tokens = semantic_tokens(CONDITIONAL, &count);
  const struct {
    int line;
    int column;
    SemanticType type;
  } want[] = {
    { 6, 5,  SEM_DECORATOR   }, /* @ */
    { 6, 6,  SEM_DECORATOR   }, /* if */
    { 6, 10, SEM_KEYWORD     }, /* valof */
    { 6, 16, SEM_PROPERTY    }, /* hdr */
    { 6, 20, SEM_PROPERTY    }, /* flag */
    { 6, 26, SEM_OPERATOR    }, /* == */
    { 6, 29, SEM_NUMBER      }, /* 1 */
    { 7, 26, SEM_OPERATOR    }, /* != */
    { 7, 29, SEM_ENUM_MEMBER }, /* RUN */
    { 7, 36, SEM_PROPERTY    }, /* f, after the group */
  };
  int failure = tokens == NULL ? __LINE__ : 0;
  for (size_t w = 0; failure == 0 && w < sizeof(want) / sizeof(*want); ++w) {
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
      if (tokens[i].line == want[w].line && tokens[i].column == want[w].column) {
        found = tokens[i].type == want[w].type;
      }
    }
    failure = found ? 0 : __LINE__ * 100 + (int)w;
  }
  free(tokens);
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
  ADD_TEST_CASE(us15_tc08_an_enum_and_its_names_are_navigable, "an enum and its names hover and lead home", false);
  ADD_TEST_CASE(us15_tc09_completion_offers_enums_and_their_names, "completion offers enums and their names", false);
  ADD_TEST_CASE(
    us15_tc10_semantic_tokens_know_an_enum_from_a_block, "semantic tokens know an enum from a block", false
  );
  ADD_TEST_CASE(us15_tc11_a_condition_is_navigable, "a condition is navigable", false);
  ADD_TEST_CASE(us15_tc12_semantic_tokens_read_a_condition, "semantic tokens read a condition", false);
}
