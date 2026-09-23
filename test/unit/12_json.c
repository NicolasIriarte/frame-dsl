/* Story 12: the JSON the language server speaks, and positions in a document (#30).
 *
 * The reader is the only part of the server an editor can hand arbitrary bytes to, so what it refuses
 * matters as much as what it accepts. The position conversions are the other half: the compiler counts
 * bytes from 1 and the protocol counts UTF-16 units from 0, and every range an editor draws goes
 * through that. */
#include <string.h>

#include "json.h"
#include "lsp_text.h"
#include "testsuite.h"

static Json *parse(Region *region, const char *text) {
  return json_parse(region, text, strlen(text));
}

static int us12_tc01_reads_the_shapes(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  int failure = 0;
  const Json *value = parse(region, "{\"a\":[1,true,null,{\"b\":\"c\"}],\"d\":-2.5e3}");

  if (value == NULL || value->kind != JSON_OBJECT || value->count != 2) {
    failure = __LINE__;
  } else {
    const Json *a = json_get(value, "a");
    const Json *d = json_get(value, "d");
    int64_t one = 0;
    if (a == NULL || a->kind != JSON_ARRAY || a->count != 4 || !json_integer(a->items[0], &one) || one != 1
        || a->items[1]->kind != JSON_TRUE || a->items[2]->kind != JSON_NULL
        || strcmp(json_string(json_get(a->items[3], "b")), "c") != 0) {
      failure = __LINE__;
    } else if (d == NULL || d->kind != JSON_NUMBER || d->number != -2500.0) {
      failure = __LINE__;
    } else if (json_get(value, "missing") != NULL || json_string(a) != NULL) {
      failure = __LINE__;
    }
  }
  region_free(region);
  return failure;
}

static int us12_tc02_reads_strings_and_escapes(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  int failure = 0;
  const struct {
    const char *text;
    const char *expected;
  } cases[] = {
    { "\"a\\\"b\\\\c\\/d\"",       "a\"b\\c/d"             },
    { "\"\\b\\f\\n\\r\\t\"",       "\b\f\n\r\t"            },
    { "\"\\u0041\\u00e9\\u20ac\"", "A\xc3\xa9\xe2\x82\xac" },
    /* A surrogate pair is one character; half of one is not a character at all. */
    { "\"\\ud83d\\ude00\"",        "\xf0\x9f\x98\x80"      },
    { "\"\\ud83d\"",               "\xef\xbf\xbd"          },
    { "\"caf\xc3\xa9\"",           "caf\xc3\xa9"           },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases) && failure == 0; ++i) {
    const Json *value = parse(region, cases[i].text);
    const char *string = json_string(value);
    if (string == NULL || strcmp(string, cases[i].expected) != 0) {
      failure = __LINE__ * 100 + (int)i;
    }
  }
  region_free(region);
  return failure;
}

static int us12_tc03_refuses_what_is_not_json(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  const char *bad[] = {
    "",
    "{",
    "}",
    "[1,]",
    "[1 2]",
    "{\"a\"}",
    "{a:1}",
    "{\"a\":}",
    "01",
    "1.",
    ".5",
    "+1",
    "- 1",
    "1e",
    "nan",
    "tru",
    "\"open",
    "\"a\nb\"",
    "\"\\x\"",
    "\"\\u00\"",
    "{} {}",
    "[1] junk",
    "\"a\"\"b\"",
    /* Written as JSON allows, and not a number a double can hold; found by fuzzing. */
    "6e466",
    "-1e400",
  };
  int failure = 0;
  for (size_t i = 0; i < sizeof(bad) / sizeof(*bad) && failure == 0; ++i) {
    if (parse(region, bad[i]) != NULL) {
      failure = __LINE__ * 100 + (int)i;
    }
  }
  /* Deeper than the reader goes, which is the reason it has a limit at all: without one this is a
   * recursion as deep as the input is long. */
  char deep[300];
  size_t used = 0;
  for (size_t i = 0; i < 80; ++i) {
    deep[used++] = '[';
  }
  deep[used++] = '1';
  for (size_t i = 0; i < 80; ++i) {
    deep[used++] = ']';
  }
  deep[used] = '\0';
  if (json_parse(region, deep, used) != NULL) {
    failure = __LINE__;
  }

  /* A string with a NUL in it is legal JSON, and stays the length it was written with. */
  const Json *nul = parse(region, "\"a\\u0000b\"");
  if (nul == NULL || nul->length != 3) {
    failure = __LINE__;
  }
  region_free(region);
  return failure;
}

static int us12_tc04_writes_what_it_reads(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  JsonWriter writer;
  json_writer_init(&writer);

  json_object_begin(&writer);
  json_key(&writer, "id");
  json_write_integer(&writer, 7);
  json_key(&writer, "list");
  json_array_begin(&writer);
  json_write_cstring(&writer, "a\"b\nc");
  json_write_bool(&writer, false);
  json_write_null(&writer);
  json_object_begin(&writer);
  json_key(&writer, "deep");
  json_write_integer(&writer, -1);
  json_object_end(&writer);
  json_array_end(&writer);
  json_object_end(&writer);

  int failure = 0;
  if (writer.failed || strcmp(writer.data, "{\"id\":7,\"list\":[\"a\\\"b\\nc\",false,null,{\"deep\":-1}]}") != 0) {
    failure = __LINE__;
  }

  /* And back again: what was written reads as what it was. */
  const Json *value = parse(region, writer.data);
  int64_t id = 0;
  if (failure == 0 && (!json_integer(json_get(value, "id"), &id) || id != 7)) {
    failure = __LINE__;
  }

  /* Control characters have to be escaped for the text to be JSON at all. */
  JsonWriter control;
  json_writer_init(&control);
  json_write_cstring(&control, "\x01\x1f");
  if (failure == 0 && strcmp(control.data, "\"\\u0001\\u001f\"") != 0) {
    failure = __LINE__;
  }
  json_writer_free(&control);
  json_writer_free(&writer);
  region_free(region);
  return failure;
}

static int us12_tc05_echoes_a_value_as_it_was(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  /* A request's id comes back exactly as it went out, whichever of the two shapes it is. */
  const char *ids[] = { "12", "\"abc\"", "null", "[1,{\"a\":\"b\"}]" };
  int failure = 0;
  for (size_t i = 0; i < sizeof(ids) / sizeof(*ids) && failure == 0; ++i) {
    JsonWriter writer;
    json_writer_init(&writer);
    json_write_value(&writer, parse(region, ids[i]));
    if (writer.failed || strcmp(writer.data, ids[i]) != 0) {
      failure = __LINE__ * 100 + (int)i;
    }
    json_writer_free(&writer);
  }
  region_free(region);
  return failure;
}

static int us12_tc06_integers_are_only_integers(const TestArguments *args) {
  (void)args;
  Region *region = region_new();
  int64_t out = 0;
  int failure = 0;
  if (!json_integer(parse(region, "-9007199254740992"), &out) || out != -9007199254740992) {
    failure = __LINE__;
  } else if (json_integer(parse(region, "1.5"), &out) || json_integer(parse(region, "1e300"), &out)
             || json_integer(parse(region, "\"1\""), &out) || json_integer(NULL, &out)) {
    failure = __LINE__;
  }
  region_free(region);
  return failure;
}

/* ---- Positions ---------------------------------------------------------------------------------- */

static int us12_tc07_columns_count_what_the_editor_counts(const TestArguments *args) {
  (void)args;
  /* "é" is two bytes and one UTF-16 unit; the emoji is four bytes and two -- a surrogate pair. */
  const char *source = "pkt A {\n  # caf\xc3\xa9 \xf0\x9f\x98\x80 x\n};\n";
  LspText utf16;
  LspText utf8;
  if (!lsp_text_init(&utf16, source, strlen(source), true)) {
    return __LINE__;
  }
  if (!lsp_text_init(&utf8, source, strlen(source), false)) {
    lsp_text_free(&utf16);
    return __LINE__;
  }

  int failure = 0;
  size_t line = 0;
  size_t character = 0;
  /* The `x` is at byte column 16 of line 2 to the compiler: 12 units into the line to an editor
   * counting UTF-16, and 15 to one counting bytes. */
  lsp_text_position(&utf16, 2, 16, &line, &character);
  if (line != 1 || character != 12) {
    failure = __LINE__;
  }
  lsp_text_position(&utf8, 2, 16, &line, &character);
  if (failure == 0 && (line != 1 || character != 15)) {
    failure = __LINE__;
  }

  /* And back: the editor's position is the byte it started from. */
  if (failure == 0 && lsp_text_offset(&utf16, 1, 12) != (size_t)(strchr(source, 'x') - source)) {
    failure = __LINE__;
  }
  if (failure == 0 && lsp_text_offset(&utf8, 1, 15) != (size_t)(strchr(source, 'x') - source)) {
    failure = __LINE__;
  }

  /* Past the end of a line, or of the file, is clamped rather than run off. */
  lsp_text_position(&utf16, 99, 99, &line, &character);
  if (failure == 0 && line != 3) {
    failure = __LINE__;
  }
  if (failure == 0 && lsp_text_offset(&utf16, 0, 999) != 7) {
    failure = __LINE__;
  }

  lsp_text_free(&utf8);
  lsp_text_free(&utf16);
  return failure;
}

void register_user_story12(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us12_tc01_reads_the_shapes, "objects, arrays, numbers and literals are read", false);
  ADD_TEST_CASE(us12_tc02_reads_strings_and_escapes, "escapes, \\u and surrogate pairs decode to UTF-8", false);
  ADD_TEST_CASE(us12_tc03_refuses_what_is_not_json, "what is not JSON is refused, depth included", false);
  ADD_TEST_CASE(us12_tc04_writes_what_it_reads, "the writer nests, separates and escapes", false);
  ADD_TEST_CASE(us12_tc05_echoes_a_value_as_it_was, "a value is echoed back exactly as it came", false);
  ADD_TEST_CASE(us12_tc06_integers_are_only_integers, "only whole numbers in range are integers", false);
  ADD_TEST_CASE(us12_tc07_columns_count_what_the_editor_counts, "positions convert both ways, and clamp", false);
}
