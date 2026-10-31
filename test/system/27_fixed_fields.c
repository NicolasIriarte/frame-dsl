/* Story 27: a fixed field cannot be written (#140). The encoder writes the definition's value whatever
 * a program does, so a write it accepted would be reported as done and never reach the wire: a test
 * could pass without injecting the fault it set up. */
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

/* Records the first failing line and carries on, so a case reads as a list of expectations. */
#define EXPECT(condition)                                                                                              \
  do {                                                                                                                 \
    if (failure == 0 && !(condition)) {                                                                                \
      failure = __LINE__;                                                                                              \
    }                                                                                                                  \
  } while (0)

static const char DEFINITION[] =
  "enum Mode: 8 { OFF; ON; };\n"
  "cal twice(raw) = raw * 2;\n"
  "cal square(raw) = raw * raw;\n"
  "block Item { mark: 4 = 0xA; value: 4; };\n"
  "pkt P {\n"
  "  id:   8 = 0x10;\n"
  "  [ @cal: twice; ]\n"
  "  scale: 8 = 4;\n"
  "  [ @cal: square; ]\n"
  "  bent: 8 = 3;\n"
  "  mode: Mode = ON;\n"
  "  body: 8;\n"
  "  items: Item[2];\n"
  "};\n"
  "pkt Q {\n"
  "  id:   8 = 0x20;\n"
  "  size: 8;\n"
  "  data[4]: valof(size) * 8;\n"
  "  end:  8 = 0x7F;\n"
  "};\n";

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool fixture_open(Fixture *fixture) {
  fixture->result = framedsl_load_string(DEFINITION, "fixed.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = framedsl_frame_new(fixture->definition);
  return fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static FramedslFieldId id_of(const Fixture *fixture, const char *dotted) {
  FramedslFieldId id = { 0 };
  framedsl_find(fixture->definition, dotted, &id);
  return id;
}

/* What a field reads, or a value no field here holds if it cannot be read. */
static uint64_t get(const Fixture *fixture, const char *dotted) {
  uint64_t value = 0;
  return framedsl_get_u64(fixture->frame, id_of(fixture, dotted), &value) == FRAMEDSL_OK ? value : UINT64_MAX;
}

static const uint8_t ENCODED[] = { 0x10, 0x04, 0x03, 0x01, 0x07, 0xA0, 0xA0 };

static bool encodes_as_defined(Fixture *fixture) {
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, "P", &length, NULL);
  return bytes != NULL && length == sizeof(ENCODED) && memcmp(bytes, ENCODED, length) == 0;
}

static int us27_tc01_every_setter_refuses_a_fixed_field(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  FramedslFrame *frame = fixture.frame;

  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.body"), 7) == FRAMEDSL_OK);

  /* The value it already has is refused like any other, so there is one rule. */
  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.id"), 0x10) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.id"), 0x55) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_name(frame, id_of(&fixture, "P.mode"), "ON") == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_name(frame, id_of(&fixture, "P.mode"), "OFF") == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_eng(frame, id_of(&fixture, "P.scale"), 8.0) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_eng(frame, id_of(&fixture, "P.scale"), 20.0) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.items[1].mark"), 0xA) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.items[1].mark"), 0x3) == FRAMEDSL_ERROR_FIXED);

  /* Nothing moved. */
  EXPECT(get(&fixture, "P.id") == 0x10 && get(&fixture, "P.scale") == 4 && get(&fixture, "P.mode") == 1);
  EXPECT(get(&fixture, "P.items[1].mark") == 0xA);
  EXPECT(encodes_as_defined(&fixture));

  fixture_close(&fixture);
  return failure;
}

/* A bad id and the wrong accessor are asked about first, and the fixed value before anything about what
 * was passed: too wide, an unlisted name, and a curve that cannot be run backwards. */
static int us27_tc02_the_order_of_the_answers(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  FramedslFrame *frame = fixture.frame;
  const uint8_t byte = 0;

  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.id"), 0x1FF) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_u64(frame, id_of(&fixture, "P.items[0].mark"), 0xFF) == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_name(frame, id_of(&fixture, "P.mode"), "NOT_A_NAME") == FRAMEDSL_ERROR_FIXED);
  EXPECT(framedsl_set_eng(frame, id_of(&fixture, "P.bent"), 9.0) == FRAMEDSL_ERROR_FIXED);

  EXPECT(framedsl_set_bytes(frame, id_of(&fixture, "P.id"), &byte, 1) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_set_eng(frame, id_of(&fixture, "P.id"), 1.0) == FRAMEDSL_ERROR_FIELD);
  EXPECT(framedsl_set_name(frame, id_of(&fixture, "P.id"), "ON") == FRAMEDSL_ERROR_FIELD);

  FramedslFieldId nothing = id_of(&fixture, "P.id");
  nothing.field = 1000;
  EXPECT(framedsl_set_u64(frame, nothing, 0) == FRAMEDSL_ERROR_FIELD);

  fixture_close(&fixture);
  return failure;
}

static int us27_tc03_a_fixed_field_reads_its_value_on_a_frame_being_built(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;

  EXPECT(get(&fixture, "P.id") == 0x10 && get(&fixture, "P.mode") == 1);
  EXPECT(get(&fixture, "P.items[0].mark") == 0xA && get(&fixture, "P.items[1].mark") == 0xA);
  EXPECT(get(&fixture, "P.body") == 0 && get(&fixture, "P.items[1].value") == 0);

  const char *name = NULL;
  double engineering = 0;
  EXPECT(framedsl_get_name(fixture.frame, id_of(&fixture, "P.mode"), &name) == FRAMEDSL_OK && strcmp(name, "ON") == 0);
  EXPECT(
    framedsl_get_eng(fixture.frame, id_of(&fixture, "P.scale"), &engineering) == FRAMEDSL_OK && engineering == 8.0
  );

  /* A reset puts them back, with everything else at zero. */
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.body"), 7) == FRAMEDSL_OK);
  EXPECT(framedsl_set_u64(fixture.frame, id_of(&fixture, "P.items[1].value"), 5) == FRAMEDSL_OK);
  framedsl_frame_reset(fixture.frame);
  EXPECT(get(&fixture, "P.id") == 0x10 && get(&fixture, "P.items[1].mark") == 0xA);
  EXPECT(get(&fixture, "P.body") == 0 && get(&fixture, "P.items[1].value") == 0);

  fixture_close(&fixture);
  return failure;
}

/* Past the known prefix a fixed value identifies nothing, so a frame that differs there decodes. The
 * field reads what arrived, and a decode that fails leaves it reading what it did. */
static int us27_tc04_a_decoded_fixed_field_reads_what_arrived(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!fixture_open(&fixture)) {
    fixture_close(&fixture);
    FORCE_FAILURE();
  }
  int failure = 0;
  const uint8_t differs[] = { 0x20, 0x01, 0xAA, 0x00 };
  const uint8_t truncated[] = { 0x20, 0x03, 0xAA };
  FramedslStatus status = FRAMEDSL_OK;

  EXPECT(get(&fixture, "Q.end") == 0x7F);
  framedsl_decode(fixture.frame, truncated, sizeof(truncated), NULL, &status);
  EXPECT(status == FRAMEDSL_ERROR_TRUNCATED && get(&fixture, "Q.end") == 0x7F);

  EXPECT(framedsl_decode(fixture.frame, differs, sizeof(differs), NULL, &status) != NULL && status == FRAMEDSL_OK);
  EXPECT(get(&fixture, "Q.end") == 0x00);

  /* Encoding still writes the definition's value: a frame identifies itself. */
  size_t length = 0;
  const uint8_t *bytes = framedsl_encode(fixture.frame, "Q", &length, NULL);
  EXPECT(bytes != NULL && length == sizeof(differs) && bytes[length - 1] == 0x7F);

  fixture_close(&fixture);
  return failure;
}

void register_user_story27(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us27_tc01_every_setter_refuses_a_fixed_field, "every setter refuses a fixed field", false);
  ADD_TEST_CASE(us27_tc02_the_order_of_the_answers, "a bad id comes first, then the fixed value", false);
  ADD_TEST_CASE(
    us27_tc03_a_fixed_field_reads_its_value_on_a_frame_being_built,
    "a fixed field reads its value on a frame being built",
    false
  );
  ADD_TEST_CASE(us27_tc04_a_decoded_fixed_field_reads_what_arrived, "a decoded fixed field reads what arrived", false);
}
