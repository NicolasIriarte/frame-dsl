/* Story 22: enumerations (#11) beyond one frame's fields -- the element of a repeated block, the C that
 * --emit-c writes, the formatter's layout and the unused-declaration warning. Story 21 has the fields
 * themselves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "framedsl.h"
#include "testsuite.h"

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool fixture_open(Fixture *fixture, const char *source) {
  fixture->result = framedsl_load_string(source, "enums.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *name) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, name, &id);
  return id;
}

static bool encodes_to(Fixture *fixture, const char *packet, const uint8_t *want, size_t want_length) {
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, NULL);
  return bytes != NULL && length == want_length && memcmp(bytes, want, length) == 0;
}

static int run_capture(const char *command, char *output, size_t capacity) {
  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return -1;
  }
  size_t used = fread(output, 1, capacity - 1, pipe);
  output[used] = '\0';
  int closed = pclose(pipe);
  return WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
}

static bool write_file(const TestArguments *args, const char *name, const char *content) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", args->tmp_dir, name);
  FILE *handle = fopen(path, "w");
  if (handle == NULL) {
    return false;
  }
  fputs(content, handle);
  fclose(handle);
  return true;
}

static int us22_tc01_a_repeated_blocks_element_has_names_too(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  const char *source =
    "enum PowerState: 1 { OFF; ON; };\n"
    "block Channel { power: PowerState; level: 7; };\n"
    "pkt TM { id: 8 := 0x30; ch: Channel[2]; };\n";
  if (!fixture_open(&fixture, source)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  FramedslFieldId power = id_of(&fixture, "TM.ch[1].power");
  const char *name = NULL;
  FramedslField member;
  const uint8_t want[] = { 0x30, 0x00, 0x80 };
  int failure = 0;
  if (framedsl_set_name(fixture.frame, power, "ON") != FRAMEDSL_OK
      || framedsl_get_name(fixture.frame, power, &name) != FRAMEDSL_OK || strcmp(name, "ON") != 0) {
    failure = __LINE__;
  } else if (!encodes_to(&fixture, "TM", want, sizeof(want))) {
    failure = __LINE__;
  } else if (!framedsl_member(fixture.definition, 0, 1, 0, &member) || member.enumeration == NULL
             || strcmp(member.enumeration, "PowerState") != 0) {
    failure = __LINE__;
  }
  fixture_close(&fixture);
  return failure;
}

static int us22_tc02_the_emitted_c_names_every_value(const TestArguments *args) {
  Fixture fixture;
  const char *source =
    "enum TxLevel: 3 { OFF; LOW; MID; HIGH; MAX = 7; };\n"
    "enum Big: 64 { LOW; TOP = 0xFFFFFFFFFFFFFFF0; };\n"
    "pkt P { id: 5 := 1; level: TxLevel; big: Big; };\n";
  if (!fixture_open(&fixture, source)) {
    fixture_close(&fixture);
    return __LINE__;
  }
  size_t length = framedsl_emit_c(fixture.definition, "gen_", NULL, 0);
  char *header = malloc(length + 1);
  bool written =
    length > 0 && header != NULL && framedsl_emit_c(fixture.definition, "gen_", header, length + 1) == length;
  fixture_close(&fixture);
  if (!written) {
    free(header);
    return __LINE__;
  }
  bool saved = write_file(args, "enums.h", header);
  free(header);
  if (!saved
      || !write_file(
        args,
        "main.c",
        "#include <stdio.h>\n#include \"enums.h\"\n"
        "int main(void) {\n"
        "  switch (7) {\n  case gen_TxLevel_MAX:\n    break;\n  default:\n    return 1;\n  }\n"
        "  printf(\"%d %d %s %s\\n\", (int)gen_TxLevel_MAX, (int)gen_TxLevel_LOW, gen_TxLevel_name(3),\n"
        "         gen_TxLevel_name(5) == 0 ? \"unlisted\" : \"?\");\n"
        "  printf(\"%llu %s\\n\", (unsigned long long)gen_Big_TOP, gen_Big_name(gen_Big_TOP));\n"
        "  return 0;\n"
        "}\n"
      )) {
    return __LINE__;
  }
  char command[2048];
  static char output[8192];
  snprintf(
    command,
    sizeof(command),
    "cc -std=c11 -O2 -Wall -Wextra -Werror -o %s/enums %s/main.c 2>&1 && %s/enums",
    args->tmp_dir,
    args->tmp_dir,
    args->tmp_dir
  );
  if (run_capture(command, output, sizeof(output)) != 0
      || strcmp(output, "7 1 HIGH unlisted\n18446744073709551600 TOP\n") != 0) {
    return __LINE__;
  }
  return 0;
}

static int us22_tc03_the_formatter_lays_an_enum_out(const TestArguments *args) {
  (void)args;
  char *out = NULL;
  const char *want =
    "enum E: 2 {\n"
    "  A;\n"
    "  B     = 2;\n"
    "  CHUNK = 3;\n"
    "};\n"
    "\n"
    "pkt P {\n"
    "  id: 6 := 1;\n"
    "  e:  E := B;\n"
    "};\n";
  if (framedsl_format("enum E:2{A;B=2;CHUNK=3;};pkt P{id:6:=1;e:E:=B;};", &out) != FRAMEDSL_OK
      || strcmp(out, want) != 0) {
    free(out);
    return __LINE__;
  }
  free(out);
  out = NULL;
  int failure = framedsl_format(want, &out) == FRAMEDSL_OK && strcmp(out, want) == 0 ? 0 : __LINE__;
  free(out);
  return failure;
}

static int us22_tc04_an_enum_nothing_uses_is_a_warning(const TestArguments *args) {
  (void)args;
  FramedslResult *result = framedsl_load_string("enum Spare: 2 {\n  A;\n};\npkt P { id: 8 := 1; };\n", "e.fdsl");
  const FramedslDiagnostic *warning = framedsl_diagnostic_count(result) == 1 ? framedsl_diagnostic(result, 0) : NULL;
  int failure = 0;
  if (!framedsl_ok(result) || warning == NULL || warning->check != 30 || warning->severity != FRAMEDSL_SEVERITY_WARNING
      || strstr(warning->message, "enum 'Spare'") == NULL) {
    failure = __LINE__;
  } else if (warning->fix_title == NULL || warning->fix_line != 1 || warning->fix_column != 1
             || warning->fix_end_line != 4 || warning->fix_end_column != 1) {
    failure = __LINE__; /* the fix removes its lines whole, as a block's does */
  }
  framedsl_result_free(result);

  /* A file with no packets is a library, and an enum in it is there for somebody else. */
  result = failure == 0 ? framedsl_load_string("enum Spare: 2 { A; };\n", "lib.fdsl") : NULL;
  if (failure == 0 && (!framedsl_ok(result) || framedsl_diagnostic_count(result) != 0)) {
    failure = __LINE__;
  }
  framedsl_result_free(result);
  return failure;
}

void register_user_story22(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us22_tc01_a_repeated_blocks_element_has_names_too, "a repeated block's element has names too", false);
  ADD_TEST_CASE(us22_tc02_the_emitted_c_names_every_value, "the emitted C names every value", false);
  ADD_TEST_CASE(us22_tc03_the_formatter_lays_an_enum_out, "the formatter lays an enum out", false);
  ADD_TEST_CASE(us22_tc04_an_enum_nothing_uses_is_a_warning, "an enum nothing uses is a warning", false);
}
