/* Story 14: a named block pasted in without an instance name.
 *
 * `hdr: Header { ... };` keeps the block's fields under `hdr.`; `Header { ... };` pastes them in under
 * their own names, as if they had been written out. Composition does the work, so most of these cases
 * look at the fields compose_file() produces and at the check-2 clashes the new form makes possible.
 * The rest follow the form through the tools that read source rather than the composed tree: the
 * unused-declaration lint, the formatter and the language server's highlighting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "compose.h"
#include "diag.h"
#include "format.h"
#include "lint.h"
#include "lsp_semantic.h"
#include "parser.h"
#include "testsuite.h"

#define HEADER "block Header { id: 8; sequence: 16; };\n"

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

/* Whether the first packet composes to exactly the fields named in `want`, a NULL-terminated list. */
static bool composes_to(const char *source, const char *const *want) {
  Composed composed;
  compose_source(source, &composed);
  bool same = composed.composed && composed.file->packet_count > 0;
  if (same) {
    const AstPacket *packet = &composed.file->packets[0];
    size_t count = 0;
    while (want[count] != NULL) {
      count += 1;
    }
    same = packet->field_count == count;
    for (size_t i = 0; same && i < count; ++i) {
      same = strcmp(packet->fields[i].name, want[i]) == 0;
    }
  }
  release(&composed);
  return same;
}

/* The first diagnostic composing `source` reports with check `wanted`, or NULL. The message is copied
 * out, since the tree it came with is gone by the time the caller reads it; the next call reuses it. */
static const char *reported(const char *source, int wanted, int *line, int *column) {
  static char message[512];
  Composed composed;
  compose_source(source, &composed);
  const char *found = NULL;
  for (size_t i = 0; i < composed.diags.count && found == NULL; ++i) {
    const Diag *diag = &composed.diags.items[i];
    if (diag->check == wanted) {
      snprintf(message, sizeof(message), "%s", diag->message);
      *line = diag->line;
      *column = diag->column;
      found = message;
    }
  }
  release(&composed);
  return found;
}

static int us14_tc01_the_fields_keep_their_own_names(const TestArguments *args) {
  (void)args;
  const char *const braced[] = { "id", "sequence", "result", NULL };
  if (!composes_to(HEADER "pkt P { Header { id = 0x82; }; result: 8; };\n", braced)) {
    return __LINE__;
  }
  /* Without overrides, braces or none. */
  if (!composes_to(HEADER "pkt P { Header; result: 8; };\n", braced)
      || !composes_to(HEADER "pkt P { Header {}; result: 8; };\n", braced)) {
    return __LINE__;
  }
  /* And the named form is unchanged beside it. */
  const char *const named[] = { "hdr.id", "hdr.sequence", "result", NULL };
  if (!composes_to(HEADER "pkt P { hdr: Header { id = 0x82; }; result: 8; };\n", named)) {
    return __LINE__;
  }
  return 0;
}

static int us14_tc02_overrides_fix_the_pasted_fields(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(HEADER "pkt P { Header { id = 0x82; }; };\n", &composed);
  int failure = 0;
  if (!composed.composed) {
    failure = __LINE__;
  } else {
    const AstField *id = &composed.file->packets[0].fields[0];
    if (!id->has_fixed || id->fixed != 0x82 || composed.file->packets[0].fields[1].has_fixed) {
      failure = __LINE__;
    }
  }
  release(&composed);
  if (failure != 0) {
    return failure;
  }

  int line = 0;
  int column = 0;
  const char *message = reported(HEADER "pkt P { Header { nope = 1; }; };\n", 25, &line, &column);
  failure = message == NULL || strstr(message, "no field 'nope'") == NULL ? __LINE__ : 0;
  return failure;
}

/* Pasted into a block's declaration, and that block used by name: the names nest from the named use
 * only. */
static int us14_tc03_a_block_can_paste_in_another(const TestArguments *args) {
  (void)args;
  const char *const want[] = { "f.id", "f.sequence", "f.crc", "tail", NULL };
  if (!composes_to(
        HEADER "block Framed { Header { id = 0x10; }; crc: 16; };\n"
               "pkt P { f: Framed { id = 0x12; }; tail: 8; };\n",
        want
      )) {
    return __LINE__;
  }
  const char *const flat[] = { "id", "sequence", "crc", NULL };
  if (!composes_to(HEADER "block Framed { Header; crc: 16; };\npkt P { Framed { id = 0x11; }; };\n", flat)) {
    return __LINE__;
  }
  return 0;
}

static int us14_tc04_a_clash_with_an_earlier_field_is_reported_at_the_use(const TestArguments *args) {
  (void)args;
  int line = 0;
  int column = 0;
  const char *message = reported(HEADER "pkt P {\n  sequence: 8;\n  Header { id = 1; };\n};\n", 2, &line, &column);
  int failure = 0;
  if (message == NULL || line != 4 || column != 3) {
    failure = __LINE__;
  } else if (strstr(message, "'sequence'") == NULL || strstr(message, "line 3") == NULL) {
    failure = __LINE__; /* what clashed, and where the other one is */
  }
  return failure;
}

static int us14_tc05_a_clash_with_a_later_field_is_reported_at_the_use(const TestArguments *args) {
  (void)args;
  int line = 0;
  int column = 0;
  const char *message = reported(HEADER "pkt P {\n  Header { id = 1; };\n  sequence: 8;\n};\n", 2, &line, &column);
  int failure = message == NULL || line != 3 || strstr(message, "line 4") == NULL ? __LINE__ : 0;
  return failure;
}

/* Two anonymous uses: the clash names the earlier use's line, not the block's declaration. */
static int us14_tc06_two_pastes_that_clash_name_each_other(const TestArguments *args) {
  (void)args;
  int line = 0;
  int column = 0;
  const char *message = reported(HEADER "pkt P {\n  Header { id = 1; };\n  Header;\n};\n", 2, &line, &column);
  int failure = message == NULL || line != 4 || strstr(message, "line 3") == NULL ? __LINE__ : 0;
  if (failure != 0) {
    return failure;
  }

  /* Inside a block's declaration too, where the block is checked on its own. */
  message =
    reported(HEADER "block Outer {\n  id: 4;\n  Header;\n};\npkt P { o: Outer; x: 8 = 1; };\n", 2, &line, &column);
  failure = message == NULL || line != 4 || strstr(message, "'Outer'") == NULL ? __LINE__ : 0;
  return failure;
}

/* `seq;` is what a field missing its width looks like, so the message says what a field would need. */
static int us14_tc07_a_name_that_is_not_a_block(const TestArguments *args) {
  (void)args;
  int line = 0;
  int column = 0;
  const char *message = reported("pkt P { id: 8 = 1; seq; };\n", 23, &line, &column);
  int failure = message == NULL || strstr(message, "'seq: 8;'") == NULL ? __LINE__ : 0;
  return failure;
}

/* Refused by composition rather than the parser: until every declaration is read, the name could be an
 * enum's, and a field typed by an enum does take a group (#11). */
static int us14_tc08_attributes_in_front_are_refused(const TestArguments *args) {
  (void)args;
  Composed composed;
  compose_source(HEADER "pkt P { [ @byte_order: little_endian; ] Header { id = 1; }; };\n", &composed);
  bool found = false;
  for (size_t i = 0; i < composed.diags.count; ++i) {
    found |= strstr(composed.diags.items[i].message, "layout is fixed where the block is declared") != NULL;
  }
  release(&composed);
  return found ? 0 : __LINE__;
}

/* The unused-declaration lint reads the tree before composition, where the use is still a use. */
static int us14_tc09_a_block_used_only_anonymously_is_used(const TestArguments *args) {
  (void)args;
  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse(HEADER "block Spare { s: 8; };\npkt P { Header { id = 1; }; };\n", &diags);
  size_t parsed = diags.count;
  if (file != NULL) {
    lint_unused(file, &diags);
  }
  int failure = 0;
  if (file == NULL || parsed != 0 || diags.count != 1 || strstr(diags.items[0].message, "'Spare'") == NULL) {
    failure = __LINE__;
  }
  ast_free(file);
  diag_free(&diags);
  return failure;
}

static int us14_tc10_the_formatter_writes_one_plain_line(const TestArguments *args) {
  (void)args;
  DiagList diags;
  diag_init(&diags);
  char *formatted = format_source(
    HEADER "pkt P {\n  a_long_name: 8;\n  Header{id=1;}\n  ;\n  x : 4;\n  vendor . Header;\n};\n", &diags
  );
  const char *want =
    "block Header {\n"
    "  id:       8;\n"
    "  sequence: 16;\n"
    "};\n"
    "\n"
    "pkt P {\n"
    "  a_long_name: 8;\n"
    "  Header { id = 1; };\n"
    "  x:           4;\n"
    "  vendor.Header;\n"
    "};\n";
  int failure = formatted == NULL || strcmp(formatted, want) != 0 ? __LINE__ : 0;
  free(formatted);
  diag_free(&diags);
  return failure;
}

static bool token_is(const SemanticToken *tokens, size_t count, const char *text, SemanticType type) {
  size_t length = strlen(text);
  for (size_t i = 0; i < count; ++i) {
    if (tokens[i].length == length && memcmp(tokens[i].text, text, length) == 0) {
      return tokens[i].type == type;
    }
  }
  return false;
}

static int us14_tc11_the_language_server_highlights_the_block_name(const TestArguments *args) {
  (void)args;
  size_t count = 0;
  SemanticToken *tokens = semantic_tokens("pkt P {\n  Header { id = 1; };\n  vendor.Frame;\n  crc: 16;\n};\n", &count);
  int failure = 0;
  if (tokens == NULL) {
    failure = __LINE__;
  } else if (!token_is(tokens, count, "Header", SEM_STRUCT) || !token_is(tokens, count, "id", SEM_PROPERTY)) {
    failure = __LINE__;
  } else if (!token_is(tokens, count, "vendor", SEM_NAMESPACE) || !token_is(tokens, count, "Frame", SEM_STRUCT)) {
    failure = __LINE__;
  } else if (!token_is(tokens, count, "crc", SEM_PROPERTY)) {
    failure = __LINE__; /* and the field after it is a field again */
  }
  free(tokens);
  return failure;
}

void register_user_story14(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us14_tc01_the_fields_keep_their_own_names, "a pasted block's fields keep their own names", true);
  ADD_TEST_CASE(us14_tc02_overrides_fix_the_pasted_fields, "overrides fix the pasted fields, and are checked", false);
  ADD_TEST_CASE(us14_tc03_a_block_can_paste_in_another, "a block can paste in another, and be used by name", false);
  ADD_TEST_CASE(
    us14_tc04_a_clash_with_an_earlier_field_is_reported_at_the_use, "a clash with an earlier field is check 2", false
  );
  ADD_TEST_CASE(
    us14_tc05_a_clash_with_a_later_field_is_reported_at_the_use, "a clash with a later field is check 2", false
  );
  ADD_TEST_CASE(us14_tc06_two_pastes_that_clash_name_each_other, "two pastes that clash name each other", false);
  ADD_TEST_CASE(us14_tc07_a_name_that_is_not_a_block, "a name that is not a block says what a field needs", false);
  ADD_TEST_CASE(us14_tc08_attributes_in_front_are_refused, "an attribute group in front of a paste is refused", false);
  ADD_TEST_CASE(us14_tc09_a_block_used_only_anonymously_is_used, "a block used only anonymously is not unused", false);
  ADD_TEST_CASE(us14_tc10_the_formatter_writes_one_plain_line, "the formatter writes a paste as one line", false);
  ADD_TEST_CASE(
    us14_tc11_the_language_server_highlights_the_block_name, "semantic tokens call the pasted name a block", false
  );
}
