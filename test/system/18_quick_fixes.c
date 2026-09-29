/* Story 18: quick fixes (#73) -- the one obvious repair a diagnostic can carry.
 *
 * The check that finds a problem attaches the fix; the language server and `--check` only relay it.
 * So each case here loads a file through the public API, takes the fix off the diagnostic, applies it
 * the way an editor would, and loads the result again: a fix is only right if the file it leaves is. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "framedsl.h"
#include "testsuite.h"

static bool write_file(const TestArguments *args, const char *name, const char *content, char *path, size_t size) {
  snprintf(path, size, "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static char *slurp(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    return NULL;
  }
  fseek(handle, 0, SEEK_END);
  long size = ftell(handle);
  fseek(handle, 0, SEEK_SET);
  char *text = size >= 0 ? malloc((size_t)size + 1) : NULL;
  if (text != NULL) {
    size_t got = fread(text, 1, (size_t)size, handle);
    text[got] = '\0';
  }
  fclose(handle);
  return text;
}

/* A byte offset for a 1-based line and column, clamped to the text: a removal of the last line ends on
 * the line after it. */
static size_t offset_of(const char *text, int line, int column) {
  size_t at = 0;
  for (int l = 1; l < line && text[at] != '\0'; ++at) {
    l += text[at] == '\n';
  }
  at += (size_t)(column - 1);
  size_t length = strlen(text);
  return at < length ? at : length;
}

/* `source` with the fix on `diagnostic` applied, as a new string; NULL if it carries none. */
static char *apply(const char *source, const FramedslDiagnostic *diagnostic) {
  if (diagnostic == NULL || diagnostic->fix_title == NULL) {
    return NULL;
  }
  size_t from = offset_of(source, diagnostic->fix_line, diagnostic->fix_column);
  size_t to = offset_of(source, diagnostic->fix_end_line, diagnostic->fix_end_column);
  size_t length = strlen(source);
  size_t inserted = strlen(diagnostic->fix_text);
  char *out = to >= from ? malloc(length - (to - from) + inserted + 1) : NULL;
  if (out != NULL) {
    memcpy(out, source, from);
    memcpy(out + from, diagnostic->fix_text, inserted);
    strcpy(out + from + inserted, source + to);
  }
  return out;
}

/* The first diagnostic of check `check` (0 for a syntax error), or NULL. */
static const FramedslDiagnostic *find(const FramedslResult *result, int check) {
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    if (framedsl_diagnostic(result, i)->check == check) {
      return framedsl_diagnostic(result, i);
    }
  }
  return NULL;
}

static size_t count_of(const FramedslResult *result, FramedslSeverity severity) {
  size_t count = 0;
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    count += framedsl_diagnostic(result, i)->severity == severity;
  }
  return count;
}

/* Whether the fix on the first diagnostic of `check` in `source` is titled `title` and turns the source
 * into exactly `want`. */
static bool fixes_to(const char *source, int check, const char *title, const char *want) {
  FramedslResult *result = framedsl_load_string(source, "t.fdsl");
  const FramedslDiagnostic *diagnostic = find(result, check);
  char *fixed = apply(source, diagnostic);
  bool same = fixed != NULL && strcmp(diagnostic->fix_title, title) == 0 && strcmp(fixed, want) == 0;
  free(fixed);
  framedsl_result_free(result);
  return same;
}

/* Whether the first diagnostic of `check` in `source` exists and carries no fix. */
static bool reported_without_fix(const char *source, int check) {
  FramedslResult *result = framedsl_load_string(source, "t.fdsl");
  const FramedslDiagnostic *diagnostic = find(result, check);
  bool none = diagnostic != NULL && diagnostic->fix_title == NULL;
  framedsl_result_free(result);
  return none;
}

static int us18_tc01_a_misspelt_attribute_is_corrected(const TestArguments *args) {
  (void)args;
  if (!fixes_to(
        "@byte_ordre: big_endian;\npkt TM {\n  id: 8 = 1;\n};\n",
        0,
        "did you mean 'byte_order'?",
        "@byte_order: big_endian;\npkt TM {\n  id: 8 = 1;\n};\n"
      )) {
    return __LINE__;
  }
  if (!fixes_to(
        "pkt TM {\n  [ @bit_order: msb_frist; ] id: 8 = 1;\n};\n",
        0,
        "did you mean 'msb_first'?",
        "pkt TM {\n  [ @bit_order: msb_first; ] id: 8 = 1;\n};\n"
      )) {
    return __LINE__;
  }
  /* Nothing within two edits: no guess. */
  return reported_without_fix("@ordering: big_endian;\npkt TM {\n  id: 8 = 1;\n};\n", 0) ? 0 : __LINE__;
}

/* `lenght` is one swap from `length`. Against `length` and `lenhgt` it is one edit from each, and a fix
 * offered for either would be a coin toss. */
static int us18_tc02_a_valof_target_is_corrected_only_when_one_name_is_closest(const TestArguments *args) {
  (void)args;
  if (!fixes_to(
        "pkt TM {\n  id: 8 = 1;\n  length: 8;\n  x[4]: valof(lenght) * 8;\n};\n",
        7,
        "did you mean 'length'?",
        "pkt TM {\n  id: 8 = 1;\n  length: 8;\n  x[4]: valof(length) * 8;\n};\n"
      )) {
    return __LINE__;
  }
  if (!reported_without_fix(
        "pkt TM {\n  id: 8 = 1;\n  length: 8;\n  lenhgt: 8;\n  x[4]: valof(lenght) * 8;\n};\n", 7
      )) {
    return __LINE__;
  }
  /* Three edits from `len`: too far to be a typo. */
  return reported_without_fix("pkt TM {\n  id: 8 = 1;\n  len: 8;\n  x[4]: valof(lenght) * 8;\n};\n", 7) ? 0 : __LINE__;
}

/* Inside a block only the part after the instance's prefix is written, so only that part is replaced. */
static int us18_tc03_a_valof_inside_a_block_keeps_its_prefix_out(const TestArguments *args) {
  (void)args;
  return fixes_to(
           "block H {\n  length: 8;\n  x[4]: valof(lenght) * 8;\n};\npkt TM {\n  id: 8 = 1;\n  hdr: H;\n};\n",
           7,
           "did you mean 'length'?",
           "block H {\n  length: 8;\n  x[4]: valof(length) * 8;\n};\npkt TM {\n  id: 8 = 1;\n  hdr: H;\n};\n"
         )
           ? 0
           : __LINE__;
}

static int us18_tc04_a_misspelt_block_or_calibration_is_corrected(const TestArguments *args) {
  (void)args;
  if (!fixes_to(
        "block Header {\n  id: 8;\n};\npkt TM {\n  hdr: Heder { id = 1; };\n};\n",
        23,
        "did you mean 'Header'?",
        "block Header {\n  id: 8;\n};\npkt TM {\n  hdr: Header { id = 1; };\n};\n"
      )) {
    return __LINE__;
  }
  if (!fixes_to(
        "cal volts(x) = x * 2;\npkt TM {\n  id: 8 = 1;\n  [ @cal: vots; ]\n  v: 8;\n};\n",
        18,
        "did you mean 'volts'?",
        "cal volts(x) = x * 2;\npkt TM {\n  id: 8 = 1;\n  [ @cal: volts; ]\n  v: 8;\n};\n"
      )) {
    return __LINE__;
  }
  return fixes_to(
           "cal half(x) = x / 2;\ncal volts(x) = hlaf(x) + 1;\npkt TM {\n  id: 8 = 1;\n  [ @cal: volts; ]\n  v: "
           "8;\n};\n",
           18,
           "did you mean 'half'?",
           "cal half(x) = x / 2;\ncal volts(x) = half(x) + 1;\npkt TM {\n  id: 8 = 1;\n  [ @cal: volts; ]\n  v: "
           "8;\n};\n"
         )
           ? 0
           : __LINE__;
}

/* An imported name is corrected under the name this file gives it. */
static int us18_tc05_an_imported_block_is_corrected_with_its_namespace(const TestArguments *args) {
  char library[1024];
  char top[1024];
  const char *source = "import \"link.fdsl\" as link;\npkt TM {\n  hdr: link.Heder { id = 1; };\n};\n";
  if (!write_file(args, "link.fdsl", "block Header {\n  id: 8;\n};\n", library, sizeof(library))
      || !write_file(args, "top.fdsl", source, top, sizeof(top))) {
    return __LINE__;
  }
  FramedslResult *result = framedsl_load_source(source, top);
  char *fixed = apply(source, find(result, 23));
  int failure = 0;
  if (fixed == NULL || strstr(fixed, "hdr: link.Header {") == NULL) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  result = fixed != NULL ? framedsl_load_source(fixed, top) : NULL;
  if (failure == 0 && !framedsl_ok(result)) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  free(fixed);
  return failure;
}

static int us18_tc06_a_missing_semicolon_is_inserted(const TestArguments *args) {
  (void)args;
  const char *source = "pkt TM {\n  id: 8 = 1;\n  len: 8\n  data: 8;\n};\n";
  const char *want = "pkt TM {\n  id: 8 = 1;\n  len: 8;\n  data: 8;\n};\n";
  if (!fixes_to(source, 0, "insert ';'", want)) {
    return __LINE__;
  }
  FramedslResult *result = framedsl_load_string(want, "t.fdsl");
  bool ok = framedsl_ok(result) && framedsl_diagnostic_count(result) == 0;
  framedsl_result_free(result);
  return ok ? 0 : __LINE__;
}

/* The block goes with its attribute group and one of the blank lines around it, so what is left is
 * what the formatter would have written without it. */
static int us18_tc07_an_unused_block_is_removed_cleanly(const TestArguments *args) {
  (void)args;
  const char *source =
    "block Header {\n  id: 8;\n};\n\n[ @byte_order: little_endian; ]\nblock Spare {\n  s: 16;\n};\n\n"
    "pkt TM {\n  hdr: Header { id = 1; };\n};\n";
  const char *want = "block Header {\n  id: 8;\n};\n\npkt TM {\n  hdr: Header { id = 1; };\n};\n";
  if (!fixes_to(source, 30, "remove the unused block 'Spare'", want)) {
    return __LINE__;
  }
  FramedslResult *result = framedsl_load_string(want, "t.fdsl");
  char *formatted = NULL;
  int failure = 0;
  if (!framedsl_ok(result) || framedsl_diagnostic_count(result) != 0) {
    failure = __LINE__;
  } else if (framedsl_format(want, &formatted) != FRAMEDSL_OK || strcmp(formatted, want) != 0) {
    failure = __LINE__;
  }
  free(formatted);
  framedsl_result_free(result);
  return failure;
}

static int us18_tc08_an_unused_calibration_or_import_is_removed(const TestArguments *args) {
  if (!fixes_to(
        "cal volts(x) = x * 2;\n\npkt TM {\n  id: 8 = 1;\n};\n",
        30,
        "remove the unused calibration 'volts'",
        "pkt TM {\n  id: 8 = 1;\n};\n"
      )) {
    return __LINE__;
  }

  char library[1024];
  char top[1024];
  const char *source = "import \"lib.fdsl\";\n\npkt TM {\n  id: 8 = 1;\n};\n";
  if (!write_file(args, "lib.fdsl", "block Header {\n  id: 8;\n};\n", library, sizeof(library))
      || !write_file(args, "top.fdsl", source, top, sizeof(top))) {
    return __LINE__;
  }
  FramedslResult *result = framedsl_load_source(source, top);
  const FramedslDiagnostic *diagnostic = find(result, 30);
  char *fixed = apply(source, diagnostic);
  int failure = 0;
  if (fixed == NULL || strcmp(diagnostic->fix_title, "remove the unused import") != 0
      || strcmp(fixed, "pkt TM {\n  id: 8 = 1;\n};\n") != 0) {
    failure = __LINE__;
  }
  free(fixed);
  framedsl_result_free(result);
  return failure;
}

/* Removing the only attribute in a group removes the group; one of two leaves the other. */
static int us18_tc09_a_redundant_attribute_is_removed_with_its_group(const TestArguments *args) {
  (void)args;
  if (!fixes_to(
        "@bit_order: msb_first;\npkt TM {\n  [ @bit_order: msb_first; ]\n  id: 8 = 1;\n};\n",
        31,
        "remove the attribute group",
        "@bit_order: msb_first;\npkt TM {\n  id: 8 = 1;\n};\n"
      )) {
    return __LINE__;
  }
  return fixes_to(
           "@bit_order: msb_first;\npkt TM {\n  [ @bit_order: msb_first; @byte_order: little_endian; ]\n  id: 16 = "
           "1;\n};\n",
           31,
           "remove the redundant attribute",
           "@bit_order: msb_first;\npkt TM {\n  [ @byte_order: little_endian; ]\n  id: 16 = 1;\n};\n"
         )
           ? 0
           : __LINE__;
}

/* A use with a typo in it is exactly what makes the block it meant look unused: deleting that block
 * would make two problems of one. */
static int us18_tc10_nothing_is_offered_for_deletion_while_there_are_errors(const TestArguments *args) {
  (void)args;
  return reported_without_fix("block Header {\n  id: 8;\n};\npkt TM {\n  hdr: Heder { id = 1; };\n};\n", 30) ? 0
                                                                                                             : __LINE__;
}

/* Every fix offered for every file under examples/invalid/, each applied alone, leaves no more errors
 * than there were -- and at least one of them is offered, or the case proves nothing. */
static int us18_tc11_no_fix_makes_an_invalid_example_worse(const TestArguments *args) {
  (void)args;
  DIR *directory = opendir(EXAMPLES_DIR "/invalid");
  if (directory == NULL) {
    return __LINE__;
  }
  int failure = 0;
  size_t offered = 0;
  struct dirent *entry;
  while (failure == 0 && (entry = readdir(directory)) != NULL) {
    size_t length = strlen(entry->d_name);
    if (length < 5 || strcmp(entry->d_name + length - 5, ".fdsl") != 0) {
      continue;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/invalid/%s", EXAMPLES_DIR, entry->d_name);
    char *source = slurp(path);
    FramedslResult *before = source != NULL ? framedsl_load_source(source, path) : NULL;
    size_t errors = count_of(before, FRAMEDSL_SEVERITY_ERROR);
    for (size_t i = 0; failure == 0 && i < framedsl_diagnostic_count(before); ++i) {
      char *fixed = apply(source, framedsl_diagnostic(before, i));
      if (fixed == NULL) {
        continue;
      }
      offered += 1;
      FramedslResult *after = framedsl_load_source(fixed, path);
      if (count_of(after, FRAMEDSL_SEVERITY_ERROR) > errors) {
        failure = __LINE__;
        fprintf(stderr, "fixing %s made it worse\n", path);
      }
      framedsl_result_free(after);
      free(fixed);
    }
    framedsl_result_free(before);
    free(source);
  }
  closedir(directory);
  return failure != 0 ? failure : offered > 0 ? 0 : __LINE__;
}

/* Check 34's fix removes the earlier of the two, on a shared line or its own, and in the file's defaults.
 * The later one is what was in effect, so the fixed file loads with the same layout and the same curve,
 * and without the warning. */
static int us18_tc12_the_earlier_of_an_attribute_written_twice_is_removed(const TestArguments *args) {
  (void)args;
  const char *cases[][2] = {
    { "pkt TM {\n  id: 8 = 1;\n  [ @byte_order: little_endian; @byte_order: big_endian; ]\n  x: 16;\n};\n",
     "pkt TM {\n  id: 8 = 1;\n  [ @byte_order: big_endian; ]\n  x: 16;\n};\n"    },
    { "cal c(x) = x * 2;\ncal k(x) = x * 3;\npkt TM {\n  id: 8 = 1;\n  [\n    @cal: k;\n    @cal: c;\n  ]\n  x: 8;\n"
      "  [ @cal: k; ]\n  y: 8;\n};\n",                                                                 "cal c(x) = x * 2;\ncal k(x) = x * 3;\npkt TM {\n  id: 8 = 1;\n  [\n    @cal: c;\n  ]\n  x: 8;\n"
      "  [ @cal: k; ]\n  y: 8;\n};\n" },
    { "@byte_order: little_endian;\n@byte_order: big_endian;\npkt TM {\n  id: 8 = 1;\n  x: 16;\n};\n",
     "@byte_order: big_endian;\npkt TM {\n  id: 8 = 1;\n  x: 16;\n};\n"          },
  };
  const char *titles[] = { "remove the earlier '@byte_order'",
                           "remove the earlier '@cal'",
                           "remove the earlier '@byte_order'" };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    if (!fixes_to(cases[i][0], 34, titles[i], cases[i][1])) {
      return __LINE__ * 10 + (int)i;
    }
    FramedslResult *before = framedsl_load_string(cases[i][0], "t.fdsl");
    FramedslResult *after = framedsl_load_string(cases[i][1], "t.fdsl");
    FramedslField was;
    FramedslField is;
    bool same = framedsl_ok(after) && find(after, 34) == NULL && count_of(after, FRAMEDSL_SEVERITY_WARNING) == 0
                && framedsl_field(framedsl_definition(before), 0, 1, &was)
                && framedsl_field(framedsl_definition(after), 0, 1, &is) && was.byte_order == is.byte_order
                && was.bit_order == is.bit_order && (was.cal == NULL) == (is.cal == NULL)
                && (was.cal == NULL || strcmp(was.cal, is.cal) == 0);
    framedsl_result_free(before);
    framedsl_result_free(after);
    if (!same) {
      return __LINE__ * 10 + (int)i;
    }
  }
  return 0;
}

void register_user_story18(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us18_tc01_a_misspelt_attribute_is_corrected, "a misspelt attribute name or value is corrected", false);
  ADD_TEST_CASE(
    us18_tc02_a_valof_target_is_corrected_only_when_one_name_is_closest,
    "valof() is corrected only when one name is closest",
    false
  );
  ADD_TEST_CASE(
    us18_tc03_a_valof_inside_a_block_keeps_its_prefix_out, "a valof() in a block is corrected as written", false
  );
  ADD_TEST_CASE(
    us18_tc04_a_misspelt_block_or_calibration_is_corrected, "a misspelt block or calibration is corrected", false
  );
  ADD_TEST_CASE(
    us18_tc05_an_imported_block_is_corrected_with_its_namespace,
    "an imported block is corrected under its namespace",
    false
  );
  ADD_TEST_CASE(us18_tc06_a_missing_semicolon_is_inserted, "a missing ';' is inserted and the error goes", false);
  ADD_TEST_CASE(
    us18_tc07_an_unused_block_is_removed_cleanly, "an unused block is removed, leaving the file formatted", false
  );
  ADD_TEST_CASE(
    us18_tc08_an_unused_calibration_or_import_is_removed, "an unused calibration or import is removed", false
  );
  ADD_TEST_CASE(
    us18_tc09_a_redundant_attribute_is_removed_with_its_group,
    "a redundant attribute goes, and its group if alone",
    false
  );
  ADD_TEST_CASE(
    us18_tc10_nothing_is_offered_for_deletion_while_there_are_errors,
    "no removal is offered while there are errors",
    false
  );
  ADD_TEST_CASE(us18_tc11_no_fix_makes_an_invalid_example_worse, "no fix makes an invalid example worse", false);
  ADD_TEST_CASE(
    us18_tc12_the_earlier_of_an_attribute_written_twice_is_removed,
    "of an attribute written twice, the earlier goes",
    false
  );
}
