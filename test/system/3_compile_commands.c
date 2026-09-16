/* Story 3: compile_commands.json, as a language server would find it.
 *
 * Deliberately shallow -- substring checks, not a JSON parser. The properties worth guarding here are
 * the ones that broke while this was being written, and both are visible as text: the quotes around a
 * define value surviving the journey from the Makefile, and the system-test entries not carrying
 * -Isrc.
 *
 * What this cannot check is whether the recorded flags actually compile the files. That is done by
 * feeding each entry back to the compiler, which needs a JSON parser and so lives outside the suite. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "testsuite.h"

#define COMPILE_COMMANDS "compile_commands.json"

static char *read_compile_commands(void) {
  FILE *handle = fopen(COMPILE_COMMANDS, "rb");
  if (handle == NULL) {
    return NULL;
  }

  if (fseek(handle, 0, SEEK_END) != 0) {
    fclose(handle);
    return NULL;
  }
  long size = ftell(handle);
  if (size < 0) {
    fclose(handle);
    return NULL;
  }
  rewind(handle);

  char *text = malloc((size_t)size + 1);
  if (text == NULL) {
    fclose(handle);
    return NULL;
  }
  size_t read = fread(text, 1, (size_t)size, handle);
  fclose(handle);
  text[read] = '\0';
  return text;
}

static size_t occurrences(const char *haystack, const char *needle) {
  size_t count = 0;
  for (const char *at = haystack; (at = strstr(at, needle)) != NULL; at += strlen(needle)) {
    count += 1;
  }
  return count;
}

static int us3_tc01_exists_and_covers_every_source_file(const TestArguments *args) {
  (void)args;

  char *text = read_compile_commands();
  if (text == NULL) {
    FORCE_FAILURE();
  }

  int failure = 0;
  /* One entry per C file under src, test/unit and test/system -- including this one. */
  const char *expected[] = { "src/main.c",          "src/lexer.c",
                             "src/parser.c",        "src/check.c",
                             "src/framedsl.c",      "test/unit/1_lexer.c",
                             "test/system/1_cli.c", "test/system/3_compile_commands.c" };

  for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); ++i) {
    if (strstr(text, expected[i]) == NULL) {
      failure = __LINE__;
      break;
    }
  }

  if (failure == 0 && occurrences(text, "\"directory\"") != occurrences(text, "\"file\"")) {
    failure = __LINE__;
  }

  free(text);
  return failure;
}

/* Paths must be absolute: a language server is not started from anywhere in particular. */
static int us3_tc02_records_absolute_paths(const TestArguments *args) {
  (void)args;

  char *text = read_compile_commands();
  if (text == NULL) {
    FORCE_FAILURE();
  }

  int failure = strstr(text, "\"file\": \"/") == NULL ? __LINE__ : 0;
  if (failure == 0 && strstr(text, "\"directory\": \"/") == NULL) {
    failure = __LINE__;
  }

  free(text);
  return failure;
}

/* The bug this file exists for. The Makefile writes -DFRAMEDSL_BIN=\"...\", and the quotes have to
 * survive make's expansion, the recipe's shell, an environment variable and word-splitting. Lose
 * them and the macro expands to a bare identifier, which a language server reports as an undeclared
 * name in code that compiles perfectly well. In JSON the quotes appear escaped. */
static int us3_tc03_define_values_keep_their_quotes(const TestArguments *args) {
  (void)args;

  char *text = read_compile_commands();
  if (text == NULL) {
    FORCE_FAILURE();
  }

  int failure = 0;
  if (strstr(text, "-DFRAMEDSL_BIN=\\\"") == NULL) {
    failure = __LINE__;
  } else if (strstr(text, "-DEXAMPLES_DIR=\\\"examples\\\"") == NULL) {
    failure = __LINE__;
  }

  free(text);
  return failure;
}

/* System tests are compiled without src/ so they cannot reach inside. An editor should show the same
 * boundary, or it offers completions for code the compiler will refuse. */
static int us3_tc04_system_entries_do_not_see_src(const TestArguments *args) {
  (void)args;

  char *text = read_compile_commands();
  if (text == NULL) {
    FORCE_FAILURE();
  }

  /* Find this file's own entry and check the flags recorded for it. */
  const char *entry = strstr(text, "test/system/3_compile_commands.c");
  int failure = 0;

  if (entry == NULL) {
    failure = __LINE__;
  } else {
    const char *end = strstr(entry, "}");
    if (end == NULL) {
      failure = __LINE__;
    } else {
      size_t length = (size_t)(end - entry);
      char *chunk = malloc(length + 1);
      if (chunk == NULL) {
        failure = __LINE__;
      } else {
        memcpy(chunk, entry, length);
        chunk[length] = '\0';
        if (strstr(chunk, "\"-Isrc\"") != NULL) {
          failure = __LINE__;
        } else if (strstr(chunk, "\"-Iinclude\"") == NULL) {
          failure = __LINE__; /* but it must see the public header */
        }
        free(chunk);
      }
    }
  }

  free(text);
  return failure;
}

void register_user_story3(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us3_tc01_exists_and_covers_every_source_file, "compile_commands.json covers every source file", true);
  ADD_TEST_CASE(us3_tc02_records_absolute_paths, "paths are absolute", false);
  ADD_TEST_CASE(us3_tc03_define_values_keep_their_quotes, "define values keep their quotes through JSON", false);
  ADD_TEST_CASE(us3_tc04_system_entries_do_not_see_src, "a system test entry gets include/ but not src/", false);
}
