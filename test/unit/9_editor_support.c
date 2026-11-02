/* Story 9: the editor support and the compiler agree about what the language is.
 *
 * editors/emacs/frame-dsl-mode.el colours a buffer with regular expressions, so it carries its own copy of
 * every word the language reserves or recognises. That copy is the whole risk of the approach: the
 * compiler grows a keyword, the mode does not, and the file somebody is looking at quietly stops
 * being the file the compiler reads. Nothing fails, nothing warns, and the first symptom is an hour
 * spent wondering why a word is not highlighted.
 *
 * So the two are compared here, in both directions, each by the method that suits it:
 *
 *   - **Mode to compiler**, by running the compiler. Every word the mode calls reserved is fed to a
 *     definition as a field name and must be rejected; every attribute the mode knows is written into
 *     a definition and must load. That is a behavioural check and cannot be fooled by a table that
 *     has been renamed or moved.
 *   - **Compiler to mode**, by reading the compiler's tables out of src/. There is no way to ask a
 *     built library to list its keywords, and adding one to the public API for a test would be the
 *     tail wagging the dog. Scanning the source is the honest version of what a person would do.
 *
 * This is a unit test rather than a system one because it reaches into src/, and because it needs no
 * editor: the half of the mode that needs Emacs to check is checked by
 * editors/emacs/frame-dsl-mode-tests.el, under `make test-editors`, which a machine without Emacs skips.
 * This half runs everywhere, including in CI. */
#include <stdio.h>
#include <string.h>

#include "framedsl.h"
#include "json.h"
#include "region.h"
#include "testsuite.h"

#define MAX_SOURCE 262144
#define MAX_WORDS 64
#define MAX_WORD 64

typedef struct {
  char words[MAX_WORDS][MAX_WORD];
  size_t count;
} WordList;

static char source_buffer[MAX_SOURCE];

/* Reads a whole file into `source_buffer` and returns it, or NULL. */
static const char *slurp(const char *directory, const char *name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", directory, name);

  FILE *handle = fopen(path, "r");
  if (handle == NULL) {
    return NULL;
  }
  size_t read = fread(source_buffer, 1, sizeof(source_buffer) - 1, handle);
  fclose(handle);

  source_buffer[read] = '\0';
  return source_buffer;
}

static bool list_has(const WordList *list, const char *word) {
  for (size_t i = 0; i < list->count; ++i) {
    if (strcmp(list->words[i], word) == 0) {
      return true;
    }
  }
  return false;
}

static bool list_add(WordList *list, const char *word, size_t length) {
  if (length == 0 || length >= MAX_WORD || list->count >= MAX_WORDS) {
    return false;
  }
  char copy[MAX_WORD];
  memcpy(copy, word, length);
  copy[length] = '\0';

  if (!list_has(list, copy)) {
    memcpy(list->words[list->count], copy, length + 1);
    list->count += 1;
  }
  return true;
}

/* Every double-quoted string between `from` and `until`, in order. */
static bool collect_quoted(const char *from, const char *until, WordList *out) {
  out->count = 0;
  for (const char *at = from; at < until;) {
    const char *open = strchr(at, '"');
    if (open == NULL || open >= until) {
      break;
    }
    const char *close = strchr(open + 1, '"');
    if (close == NULL || close >= until) {
      return false;
    }
    if (!list_add(out, open + 1, (size_t)(close - open - 1))) {
      return false;
    }
    at = close + 1;
  }
  return true;
}

/* The strings inside a table, given the text that opens it and the text that closes it. */
static bool collect_table(const char *text, const char *opener, const char *closer, WordList *out) {
  const char *start = strstr(text, opener);
  if (start == NULL) {
    return false;
  }
  start += strlen(opener);
  const char *end = strstr(start, closer);
  if (end == NULL) {
    return false;
  }
  return collect_quoted(start, end, out) && out->count > 0;
}

/* Every string passed to `word_is(&which, "...")`, which is how the parser spells its attribute
 * table: there is no array to read, the words are in the conditionals that accept them. */
static bool collect_word_is(const char *text, const char *which, WordList *out) {
  char needle[64];
  snprintf(needle, sizeof(needle), "word_is(&%s, \"", which);
  size_t needle_length = strlen(needle);

  out->count = 0;
  for (const char *at = strstr(text, needle); at != NULL; at = strstr(at, needle)) {
    const char *word = at + needle_length;
    const char *close = strchr(word, '"');
    if (close == NULL || !list_add(out, word, (size_t)(close - word))) {
      return false;
    }
    at = close;
  }
  return out->count > 0;
}

/* One of the mode's `(defconst frame-dsl-... '("a" "b"))' lists. */
static bool collect_elisp_list(const char *text, const char *name, WordList *out) {
  char opener[96];
  snprintf(opener, sizeof(opener), "(defconst %s", name);
  return collect_table(text, opener, ")", out);
}

/* Every `|`-separated piece between `from` and `until`, in order -- how a word list appears inside a
 * regular expression's alternation, whether that regex came from the Vim syntax file or the TextMate
 * grammar. */
static bool collect_alternation(const char *from, const char *until, WordList *out) {
  out->count = 0;
  const char *at = from;
  while (at < until) {
    const char *bar = memchr(at, '|', (size_t)(until - at));
    const char *piece_end = (bar != NULL && bar < until) ? bar : until;
    if (!list_add(out, at, (size_t)(piece_end - at))) {
      return false;
    }
    at = piece_end + 1;
  }
  return out->count > 0;
}

/* One of `syntax keyword <group> word word ... [contained]` in the Vim syntax file: `syntax/fdsl.vim`
 * writes a word list as the words highlighted, not as a separate table, so this reads the same line
 * the highlighter uses. `contained` is Vim's own keyword, not one of the language's, and is dropped. */
static bool collect_vim_keyword_list(const char *text, const char *group, WordList *out) {
  char needle[96];
  snprintf(needle, sizeof(needle), "syntax keyword %s ", group);
  const char *start = strstr(text, needle);
  if (start == NULL) {
    return false;
  }
  start += strlen(needle);
  const char *end = strchr(start, '\n');
  if (end == NULL) {
    end = start + strlen(start);
  }

  out->count = 0;
  const char *at = start;
  while (at < end) {
    while (at < end && *at == ' ') {
      at++;
    }
    const char *word_end = at;
    while (word_end < end && *word_end != ' ') {
      word_end++;
    }
    size_t length = (size_t)(word_end - at);
    if (length > 0 && !(length == strlen("contained") && strncmp(at, "contained", length) == 0)) {
      if (!list_add(out, at, length)) {
        return false;
      }
    }
    at = word_end;
  }
  return out->count > 0;
}

/* The word list inside `repository["key"].match` in the TextMate grammar -- the same regular
 * expression that does the highlighting, the way the Vim syntax file's word list does. The names
 * this version of the language knows sit in the first parenthesised group, whether that group is the
 * whole thing (`\b(a|b|c)\b`, for `keywords` and `attribute-values`) or the first half of an
 * unnamed alternative (`(?:(a|b|c)|(...))`, for `attribute-header`'s known-name-or-error choice). */
static bool collect_json_pattern_words(const Json *repository, const char *key, WordList *out) {
  const Json *entry = json_get(repository, key);
  const Json *match = entry != NULL ? json_get(entry, "match") : NULL;
  const char *pattern = match != NULL ? json_string(match) : NULL;
  if (pattern == NULL) {
    return false;
  }

  const char *open_paren = strchr(pattern, '(');
  if (open_paren == NULL) {
    return false;
  }
  const char *group_start = open_paren + 1;
  if (strncmp(group_start, "?:(", 3) == 0) {
    group_start += 3;
  }
  const char *close_paren = strchr(group_start, ')');
  if (close_paren == NULL) {
    return false;
  }
  return collect_alternation(group_start, close_paren, out);
}

/* Parses a whole file as JSON, or NULL if it is not a file or is not valid JSON. */
static Json *load_json(Region *region, const char *directory, const char *name) {
  const char *text = slurp(directory, name);
  if (text == NULL) {
    return NULL;
  }
  return json_parse(region, text, strlen(text));
}

/* Loads a definition and says whether it was accepted. */
static bool loads(const char *source) {
  FramedslResult *result = framedsl_load_string(source, "editor.fdsl");
  bool ok = framedsl_ok(result);
  framedsl_result_free(result);
  return ok;
}

/* Whether `@name: value;` loads on a byte-wide field -- with a calibration named after the value, since
 * `@cal`'s value is the name of one. `@if` (#12) takes a comparison rather than a word, so it is tried
 * with one, and `@decode` (#146) applies to a packet, so it is tried on one; a value belongs to one
 * attribute, so most other pairings are rejected, and what a caller asks is whether every name pairs
 * with at least one. */
static bool attribute_loads(const char *name, const char *value) {
  char definition[256];
  if (strcmp(name, "if") == 0) {
    return loads("pkt A { x: 8 := 1; [ @if: valof(x) == 1; ] y: 8; };");
  }
  if (strcmp(name, "decode") == 0) {
    snprintf(definition, sizeof(definition), "[ @decode: %s; ]\npkt A { x: 8 := 1; };", value);
    return loads(definition);
  }
  snprintf(
    definition, sizeof(definition), "cal %s(x) = x;\npkt A { x: 8 := 1; [ @%s: %s; ] y: 8; };", value, name, value
  );
  return loads(definition);
}

/* ---- Mode to compiler, by running the compiler ------------------------------------------------ */

/* A word the mode paints as a keyword had better be one. `pkt A { valof: 8 = 1; };` must fail: if it
 * loads, `valof` is an ordinary name and the mode is colouring a field. */
static int us9_tc01_every_word_the_mode_reserves_is_reserved(const TestArguments *args) {
  (void)args;

  const char *mode = slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el");
  if (mode == NULL) {
    FORCE_FAILURE();
  }

  WordList keywords;
  if (!collect_elisp_list(mode, "frame-dsl-keywords", &keywords)) {
    FORCE_FAILURE();
  }

  for (size_t i = 0; i < keywords.count; ++i) {
    char definition[256];
    snprintf(definition, sizeof(definition), "pkt A { %s: 8 := 1; };", keywords.words[i]);
    if (loads(definition)) {
      return __LINE__;
    }
  }

  /* The control. Without it this case would pass just as well against a compiler that rejected
   * everything, and would be measuring nothing. */
  if (!loads("pkt A { not_a_keyword: 8 := 1; };")) {
    return __LINE__;
  }
  return 0;
}

/* Every attribute name and value the mode knows is one the parser accepts, in every combination --
 * which is cheap here, there being four of them. */
static int us9_tc02_every_attribute_the_mode_knows_is_accepted(const TestArguments *args) {
  (void)args;

  const char *mode = slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el");
  if (mode == NULL) {
    FORCE_FAILURE();
  }

  WordList names;
  WordList values;
  if (!collect_elisp_list(mode, "frame-dsl-attribute-names", &names)) {
    FORCE_FAILURE();
  }
  /* Collected from the file a second time, because the first call left its words in the shared
   * buffer this one is about to overwrite. */
  if (slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el") == NULL
      || !collect_elisp_list(source_buffer, "frame-dsl-attribute-values", &values)) {
    FORCE_FAILURE();
  }

  size_t accepted = 0;
  for (size_t n = 0; n < names.count; ++n) {
    for (size_t v = 0; v < values.count; ++v) {
      if (attribute_loads(names.words[n], values.words[v])) {
        accepted += 1;
        break;
      }
      if (v + 1 == values.count) {
        return __LINE__;
      }
    }
  }

  if (accepted != names.count) {
    return __LINE__;
  }
  /* The control again: a name the mode would paint as a typo is one the parser refuses. */
  if (loads("@byte_ordre: big_endian;\npkt A { x: 8 := 1; };")) {
    return __LINE__;
  }
  return 0;
}

/* ---- Compiler to mode, by reading the compiler ------------------------------------------------ */

/* The direction that actually catches drift: a keyword added to the lexer and not to the mode. */
static int us9_tc03_every_keyword_the_lexer_has_is_in_the_mode(const TestArguments *args) {
  (void)args;

  WordList lexer;
  if (slurp(SOURCE_DIR, "lexer.c") == NULL || !collect_table(source_buffer, "KEYWORDS[] = {", "};", &lexer)) {
    FORCE_FAILURE();
  }

  WordList mode;
  if (slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el") == NULL
      || !collect_elisp_list(source_buffer, "frame-dsl-keywords", &mode)) {
    FORCE_FAILURE();
  }

  if (lexer.count != mode.count) {
    return __LINE__;
  }
  for (size_t i = 0; i < lexer.count; ++i) {
    if (!list_has(&mode, lexer.words[i])) {
      return __LINE__;
    }
  }
  return 0;
}

static int us9_tc04_every_attribute_the_parser_has_is_in_the_mode(const TestArguments *args) {
  (void)args;

  WordList parser_names;
  WordList parser_values;
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "name", &parser_names)) {
    FORCE_FAILURE();
  }
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "value", &parser_values)) {
    FORCE_FAILURE();
  }

  WordList mode_names;
  WordList mode_values;
  if (slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el") == NULL
      || !collect_elisp_list(source_buffer, "frame-dsl-attribute-names", &mode_names)) {
    FORCE_FAILURE();
  }
  if (slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el") == NULL
      || !collect_elisp_list(source_buffer, "frame-dsl-attribute-values", &mode_values)) {
    FORCE_FAILURE();
  }

  if (parser_names.count != mode_names.count || parser_values.count != mode_values.count) {
    return __LINE__;
  }
  for (size_t i = 0; i < parser_names.count; ++i) {
    if (!list_has(&mode_names, parser_names.words[i])) {
      return __LINE__;
    }
  }
  for (size_t i = 0; i < parser_values.count; ++i) {
    if (!list_has(&mode_values, parser_values.words[i])) {
      return __LINE__;
    }
  }
  return 0;
}

/* The extraction itself, because everything above is worthless if the scan silently finds nothing.
 * A `collect_table` that returned an empty list would make every comparison above trivially true. */
static int us9_tc05_the_word_lists_are_actually_found(const TestArguments *args) {
  (void)args;

  WordList list;
  if (slurp(SOURCE_DIR, "lexer.c") == NULL || !collect_table(source_buffer, "KEYWORDS[] = {", "};", &list)) {
    return __LINE__;
  }
  /* The three the grammar names: pkt, valof, block. Asserted as a number so that a scan which
   * started matching the wrong thing is caught rather than absorbed. */
  if (list.count != 3 || !list_has(&list, "pkt") || !list_has(&list, "valof") || !list_has(&list, "block")) {
    return __LINE__;
  }

  if (slurp(EDITORS_DIR, "emacs/frame-dsl-mode.el") == NULL
      || !collect_elisp_list(source_buffer, "frame-dsl-attribute-values", &list)) {
    return __LINE__;
  }
  if (list.count != 6 || !list_has(&list, "msb_first") || !list_has(&list, "manual")) {
    return __LINE__;
  }

  /* And a list that is not there is reported as missing rather than as empty. */
  if (collect_elisp_list(source_buffer, "frame-dsl-nothing-of-the-sort", &list)) {
    return __LINE__;
  }
  return 0;
}

/* ---- Vim to compiler, and back -- editors/vim/syntax/fdsl.vim ---------------------------------- */

/* us9_tc01's question, asked of the Vim syntax file instead of the Emacs mode. */
static int us9_tc06_every_word_the_vim_syntax_reserves_is_reserved(const TestArguments *args) {
  (void)args;

  const char *syntax = slurp(EDITORS_DIR, "vim/syntax/fdsl.vim");
  if (syntax == NULL) {
    FORCE_FAILURE();
  }

  WordList keywords;
  if (!collect_vim_keyword_list(syntax, "fdslKeyword", &keywords)) {
    FORCE_FAILURE();
  }

  for (size_t i = 0; i < keywords.count; ++i) {
    char definition[256];
    snprintf(definition, sizeof(definition), "pkt A { %s: 8 := 1; };", keywords.words[i]);
    if (loads(definition)) {
      return __LINE__;
    }
  }
  if (!loads("pkt A { not_a_keyword: 8 := 1; };")) {
    return __LINE__;
  }
  return 0;
}

/* us9_tc02's question, asked of the Vim syntax file. */
static int us9_tc07_every_attribute_the_vim_syntax_knows_is_accepted(const TestArguments *args) {
  (void)args;

  const char *syntax = slurp(EDITORS_DIR, "vim/syntax/fdsl.vim");
  if (syntax == NULL) {
    FORCE_FAILURE();
  }

  WordList names;
  WordList values;
  if (!collect_vim_keyword_list(syntax, "fdslAttributeName", &names)) {
    FORCE_FAILURE();
  }
  if (slurp(EDITORS_DIR, "vim/syntax/fdsl.vim") == NULL
      || !collect_vim_keyword_list(source_buffer, "fdslAttributeValue", &values)) {
    FORCE_FAILURE();
  }

  size_t accepted = 0;
  for (size_t n = 0; n < names.count; ++n) {
    for (size_t v = 0; v < values.count; ++v) {
      if (attribute_loads(names.words[n], values.words[v])) {
        accepted += 1;
        break;
      }
      if (v + 1 == values.count) {
        return __LINE__;
      }
    }
  }

  if (accepted != names.count) {
    return __LINE__;
  }
  if (loads("@byte_ordre: big_endian;\npkt A { x: 8 := 1; };")) {
    return __LINE__;
  }
  return 0;
}

/* us9_tc03's question, asked of the Vim syntax file. */
static int us9_tc08_every_keyword_the_lexer_has_is_in_the_vim_syntax(const TestArguments *args) {
  (void)args;

  WordList lexer;
  if (slurp(SOURCE_DIR, "lexer.c") == NULL || !collect_table(source_buffer, "KEYWORDS[] = {", "};", &lexer)) {
    FORCE_FAILURE();
  }

  WordList syntax;
  if (slurp(EDITORS_DIR, "vim/syntax/fdsl.vim") == NULL
      || !collect_vim_keyword_list(source_buffer, "fdslKeyword", &syntax)) {
    FORCE_FAILURE();
  }

  if (lexer.count != syntax.count) {
    return __LINE__;
  }
  for (size_t i = 0; i < lexer.count; ++i) {
    if (!list_has(&syntax, lexer.words[i])) {
      return __LINE__;
    }
  }
  return 0;
}

/* us9_tc04's question, asked of the Vim syntax file. */
static int us9_tc09_every_attribute_the_parser_has_is_in_the_vim_syntax(const TestArguments *args) {
  (void)args;

  WordList parser_names;
  WordList parser_values;
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "name", &parser_names)) {
    FORCE_FAILURE();
  }
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "value", &parser_values)) {
    FORCE_FAILURE();
  }

  WordList syntax_names;
  WordList syntax_values;
  if (slurp(EDITORS_DIR, "vim/syntax/fdsl.vim") == NULL
      || !collect_vim_keyword_list(source_buffer, "fdslAttributeName", &syntax_names)) {
    FORCE_FAILURE();
  }
  if (slurp(EDITORS_DIR, "vim/syntax/fdsl.vim") == NULL
      || !collect_vim_keyword_list(source_buffer, "fdslAttributeValue", &syntax_values)) {
    FORCE_FAILURE();
  }

  if (parser_names.count != syntax_names.count || parser_values.count != syntax_values.count) {
    return __LINE__;
  }
  for (size_t i = 0; i < parser_names.count; ++i) {
    if (!list_has(&syntax_names, parser_names.words[i])) {
      return __LINE__;
    }
  }
  for (size_t i = 0; i < parser_values.count; ++i) {
    if (!list_has(&syntax_values, parser_values.words[i])) {
      return __LINE__;
    }
  }
  return 0;
}

/* ---- The TextMate grammar to compiler, and back -- editors/vscode/syntaxes/fdsl.tmLanguage.json - */

/* us9_tc01's question, asked of the grammar `keywords` matches. */
static int us9_tc10_every_word_the_grammar_reserves_is_reserved(const TestArguments *args) {
  (void)args;

  Region *region = region_new();
  Json *root = load_json(region, EDITORS_DIR, "vscode/syntaxes/fdsl.tmLanguage.json");
  const Json *repository = root != NULL ? json_get(root, "repository") : NULL;
  if (repository == NULL) {
    region_free(region);
    FORCE_FAILURE();
  }

  WordList keywords;
  if (!collect_json_pattern_words(repository, "keywords", &keywords)) {
    region_free(region);
    FORCE_FAILURE();
  }

  int failure = 0;
  for (size_t i = 0; i < keywords.count && failure == 0; ++i) {
    char definition[256];
    snprintf(definition, sizeof(definition), "pkt A { %s: 8 := 1; };", keywords.words[i]);
    if (loads(definition)) {
      failure = __LINE__;
    }
  }
  if (failure == 0 && !loads("pkt A { not_a_keyword: 8 := 1; };")) {
    failure = __LINE__;
  }
  region_free(region);
  return failure;
}

/* us9_tc02's question, asked of the grammar's `attribute-header` and `attribute-values` matches. */
static int us9_tc11_every_attribute_the_grammar_knows_is_accepted(const TestArguments *args) {
  (void)args;

  Region *region = region_new();
  Json *root = load_json(region, EDITORS_DIR, "vscode/syntaxes/fdsl.tmLanguage.json");
  const Json *repository = root != NULL ? json_get(root, "repository") : NULL;
  if (repository == NULL) {
    region_free(region);
    FORCE_FAILURE();
  }

  WordList names;
  WordList values;
  if (!collect_json_pattern_words(repository, "attribute-header", &names)
      || !collect_json_pattern_words(repository, "attribute-values", &values)) {
    region_free(region);
    FORCE_FAILURE();
  }

  int failure = 0;
  size_t accepted = 0;
  for (size_t n = 0; n < names.count && failure == 0; ++n) {
    for (size_t v = 0; v < values.count; ++v) {
      if (attribute_loads(names.words[n], values.words[v])) {
        accepted += 1;
        break;
      }
      if (v + 1 == values.count) {
        failure = __LINE__;
      }
    }
  }

  if (failure == 0 && accepted != names.count) {
    failure = __LINE__;
  }
  if (failure == 0 && loads("@byte_ordre: big_endian;\npkt A { x: 8 := 1; };")) {
    failure = __LINE__;
  }
  region_free(region);
  return failure;
}

/* us9_tc03's question, asked of the grammar. */
static int us9_tc12_every_keyword_the_lexer_has_is_in_the_grammar(const TestArguments *args) {
  (void)args;

  WordList lexer;
  if (slurp(SOURCE_DIR, "lexer.c") == NULL || !collect_table(source_buffer, "KEYWORDS[] = {", "};", &lexer)) {
    FORCE_FAILURE();
  }

  Region *region = region_new();
  Json *root = load_json(region, EDITORS_DIR, "vscode/syntaxes/fdsl.tmLanguage.json");
  const Json *repository = root != NULL ? json_get(root, "repository") : NULL;
  WordList grammar;
  if (repository == NULL || !collect_json_pattern_words(repository, "keywords", &grammar)) {
    region_free(region);
    FORCE_FAILURE();
  }

  int failure = 0;
  if (lexer.count != grammar.count) {
    failure = __LINE__;
  }
  for (size_t i = 0; failure == 0 && i < lexer.count; ++i) {
    if (!list_has(&grammar, lexer.words[i])) {
      failure = __LINE__;
    }
  }
  region_free(region);
  return failure;
}

/* us9_tc04's question, asked of the grammar. */
static int us9_tc13_every_attribute_the_parser_has_is_in_the_grammar(const TestArguments *args) {
  (void)args;

  WordList parser_names;
  WordList parser_values;
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "name", &parser_names)) {
    FORCE_FAILURE();
  }
  if (slurp(SOURCE_DIR, "parser.c") == NULL || !collect_word_is(source_buffer, "value", &parser_values)) {
    FORCE_FAILURE();
  }

  Region *region = region_new();
  Json *root = load_json(region, EDITORS_DIR, "vscode/syntaxes/fdsl.tmLanguage.json");
  const Json *repository = root != NULL ? json_get(root, "repository") : NULL;
  WordList grammar_names;
  WordList grammar_values;
  if (repository == NULL || !collect_json_pattern_words(repository, "attribute-header", &grammar_names)
      || !collect_json_pattern_words(repository, "attribute-values", &grammar_values)) {
    region_free(region);
    FORCE_FAILURE();
  }

  int failure = 0;
  if (parser_names.count != grammar_names.count || parser_values.count != grammar_values.count) {
    failure = __LINE__;
  }
  for (size_t i = 0; failure == 0 && i < parser_names.count; ++i) {
    if (!list_has(&grammar_names, parser_names.words[i])) {
      failure = __LINE__;
    }
  }
  for (size_t i = 0; failure == 0 && i < parser_values.count; ++i) {
    if (!list_has(&grammar_values, parser_values.words[i])) {
      failure = __LINE__;
    }
  }
  region_free(region);
  return failure;
}

/* The Vim and JSON extractions are actually finding something, the way us9_tc05 checks the Emacs
 * ones -- and, since nothing else in this file asks, that package.json and the grammar are valid
 * JSON at all. json_parse refusing either is the same failure a `.vsix` built from a typo would
 * hit, caught here instead. */
static int us9_tc14_the_vim_and_json_word_lists_are_actually_found(const TestArguments *args) {
  (void)args;

  WordList list;
  if (slurp(EDITORS_DIR, "vim/syntax/fdsl.vim") == NULL
      || !collect_vim_keyword_list(source_buffer, "fdslKeyword", &list)) {
    return __LINE__;
  }
  if (list.count != 3 || !list_has(&list, "pkt") || !list_has(&list, "valof") || !list_has(&list, "block")) {
    return __LINE__;
  }
  if (collect_vim_keyword_list(source_buffer, "fdslNothingOfTheSort", &list)) {
    return __LINE__;
  }

  Region *region = region_new();
  Json *grammar = load_json(region, EDITORS_DIR, "vscode/syntaxes/fdsl.tmLanguage.json");
  const Json *repository = grammar != NULL ? json_get(grammar, "repository") : NULL;
  if (repository == NULL) {
    region_free(region);
    return __LINE__;
  }
  if (!collect_json_pattern_words(repository, "attribute-values", &list)) {
    region_free(region);
    return __LINE__;
  }
  if (list.count != 6 || !list_has(&list, "msb_first") || !list_has(&list, "manual")) {
    region_free(region);
    return __LINE__;
  }
  if (collect_json_pattern_words(repository, "nothing-of-the-sort", &list)) {
    region_free(region);
    return __LINE__;
  }

  Json *package = load_json(region, EDITORS_DIR, "vscode/package.json");
  if (package == NULL || json_get(package, "name") == NULL) {
    region_free(region);
    return __LINE__;
  }

  region_free(region);
  return 0;
}

void register_user_story9(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us9_tc01_every_word_the_mode_reserves_is_reserved,
    "every word the mode calls a keyword is rejected as a field name",
    false
  );
  ADD_TEST_CASE(
    us9_tc02_every_attribute_the_mode_knows_is_accepted,
    "every attribute the mode knows is one the parser accepts",
    false
  );
  ADD_TEST_CASE(
    us9_tc03_every_keyword_the_lexer_has_is_in_the_mode,
    "a keyword added to the lexer and not to the mode is caught",
    false
  );
  ADD_TEST_CASE(
    us9_tc04_every_attribute_the_parser_has_is_in_the_mode,
    "an attribute added to the parser and not to the mode is caught",
    false
  );
  ADD_TEST_CASE(us9_tc05_the_word_lists_are_actually_found, "the scan finds the tables it claims to compare", true);
  ADD_TEST_CASE(
    us9_tc06_every_word_the_vim_syntax_reserves_is_reserved,
    "every word the Vim syntax file calls a keyword is rejected as a field name",
    false
  );
  ADD_TEST_CASE(
    us9_tc07_every_attribute_the_vim_syntax_knows_is_accepted,
    "every attribute the Vim syntax file knows is one the parser accepts",
    false
  );
  ADD_TEST_CASE(
    us9_tc08_every_keyword_the_lexer_has_is_in_the_vim_syntax,
    "a keyword added to the lexer and not to the Vim syntax file is caught",
    false
  );
  ADD_TEST_CASE(
    us9_tc09_every_attribute_the_parser_has_is_in_the_vim_syntax,
    "an attribute added to the parser and not to the Vim syntax file is caught",
    false
  );
  ADD_TEST_CASE(
    us9_tc10_every_word_the_grammar_reserves_is_reserved,
    "every word the TextMate grammar calls a keyword is rejected as a field name",
    false
  );
  ADD_TEST_CASE(
    us9_tc11_every_attribute_the_grammar_knows_is_accepted,
    "every attribute the TextMate grammar knows is one the parser accepts",
    false
  );
  ADD_TEST_CASE(
    us9_tc12_every_keyword_the_lexer_has_is_in_the_grammar,
    "a keyword added to the lexer and not to the TextMate grammar is caught",
    false
  );
  ADD_TEST_CASE(
    us9_tc13_every_attribute_the_parser_has_is_in_the_grammar,
    "an attribute added to the parser and not to the TextMate grammar is caught",
    false
  );
  ADD_TEST_CASE(
    us9_tc14_the_vim_and_json_word_lists_are_actually_found,
    "the scan finds the Vim and JSON tables too, and both files are valid JSON where that applies",
    true
  );
}
