/* Story 34: real frames (#145).
 *
 * Invented frames only show what the language was already designed for. These are published ones, a
 * serial link with the id in the bytes and CAN with it in the CAN ID, so each case reads a reference frame
 * to its published values, field by field, and writes those values back to the same bytes. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct {
  const char *name;
  int64_t value;
} Expected;

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_example(Fixture *fixture, const char *path) {
  fixture->result = framedsl_load_file(path);
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = fixture->definition != NULL ? framedsl_frame_new(fixture->definition) : NULL;
  return fixture->frame != NULL;
}

static void close_example(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

/* A field's value by its type, and setting one the same way. */
static bool reads(const Fixture *fixture, const char *name, int64_t expected) {
  FramedslFieldId id;
  if (!framedsl_find(fixture->definition, name, &id)) {
    return false;
  }
  int64_t number = 0;
  if (framedsl_get_i64(fixture->frame, id, &number) == FRAMEDSL_OK) {
    return number == expected;
  }
  uint64_t raw = 0;
  return framedsl_get_u64(fixture->frame, id, &raw) == FRAMEDSL_OK && raw == (uint64_t)expected;
}

static bool writes(const Fixture *fixture, const char *name, int64_t value) {
  FramedslFieldId id;
  if (!framedsl_find(fixture->definition, name, &id)) {
    return false;
  }
  if (framedsl_set_i64(fixture->frame, id, value) == FRAMEDSL_OK) {
    return true;
  }
  return framedsl_set_u64(fixture->frame, id, (uint64_t)value) == FRAMEDSL_OK;
}

/* Every field reads as published, and a frame built from nothing but those values is the same bytes. */
static int check_frame(
  Fixture *fixture,
  const char *packet,
  const uint8_t *wire,
  size_t length,
  const Expected *fields,
  size_t count
) {
  int failure = 0;
  for (size_t i = 0; i < count; ++i) {
    EXPECT(reads(fixture, fields[i].name, fields[i].value));
  }
  framedsl_frame_reset(fixture->frame);
  for (size_t i = 0; i < count; ++i) {
    EXPECT(writes(fixture, fields[i].name, fields[i].value));
  }
  size_t got = 0;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &got, NULL);
  EXPECT(bytes != NULL && got == length && memcmp(bytes, wire, length) == 0);
  return failure;
}

static int us34_tc01_each_definition_is_clean(const TestArguments *args) {
  (void)args;
  int failure = 0;
  const char *const files[] = { "examples/ublox_nav_posllh.fdsl", "examples/j1939.fdsl" };
  for (size_t i = 0; i < sizeof(files) / sizeof(*files); ++i) {
    char command[512];
    snprintf(command, sizeof(command), "%s %s --check --werror >/dev/null 2>&1", FRAMEDSL_BIN, files[i]);
    EXPECT(system(command) == 0);
  }
  return failure;
}

/* A position 58.4 degrees west and 34.6 south, whose checksum bytes are what the protocol's Fletcher
 * algorithm gives: this library reads them, and nothing here computes one. */
static int us34_tc02_a_position_from_a_serial_link(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_example(&fixture, "examples/ublox_nav_posllh.fdsl")) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0xB5, 0x62, 0x01, 0x02, 0x1C, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x00, 0xDE,
                           0x30, 0xDD, 0x80, 0x75, 0x60, 0xEB, 0xA8, 0x61, 0x00, 0x00, 0xE0, 0x2E,
                           0x00, 0x00, 0xDC, 0x05, 0x00, 0x00, 0xC4, 0x09, 0x00, 0x00, 0x36, 0x76 };
  const char *packet = framedsl_decode(fixture.frame, wire, sizeof(wire), NULL, NULL);
  EXPECT(packet != NULL && strcmp(packet, "NAV_POSLLH") == 0);

  double degrees = 0;
  FramedslFieldId lon;
  FramedslFieldId lat;
  EXPECT(framedsl_find(fixture.definition, "NAV_POSLLH.lon", &lon));
  EXPECT(framedsl_find(fixture.definition, "NAV_POSLLH.lat", &lat));
  EXPECT(framedsl_get_eng(fixture.frame, lon, &degrees) == FRAMEDSL_OK && fabs(degrees + 58.4) < 1e-9);
  EXPECT(framedsl_get_eng(fixture.frame, lat, &degrees) == FRAMEDSL_OK && fabs(degrees + 34.6) < 1e-9);

  const Expected fields[] = {
    { "NAV_POSLLH.itow",   100000     },
    { "NAV_POSLLH.lon",    -584000000 },
    { "NAV_POSLLH.lat",    -346000000 },
    { "NAV_POSLLH.height", 25000      },
    { "NAV_POSLLH.hmsl",   12000      },
    { "NAV_POSLLH.hacc",   1500       },
    { "NAV_POSLLH.vacc",   2500       },
    { "NAV_POSLLH.ck_a",   0x36       },
    { "NAV_POSLLH.ck_b",   0x76       },
  };
  const int result = check_frame(&fixture, "NAV_POSLLH", wire, sizeof(wire), fields, sizeof(fields) / sizeof(*fields));
  close_example(&fixture);
  return failure != 0 ? failure : result;
}

/* A published EEC1: the bytes that are all ones are "not available" in J1939, and reading them is the
 * program's business. */
static int us34_tc03_an_engine_speed_from_can(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_example(&fixture, "examples/j1939.fdsl")) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0xFF, 0xFF, 0xFF, 0x68, 0x13, 0xFF, 0xFF, 0xFF };
  size_t consumed = 0;
  EXPECT(framedsl_decode_as(fixture.frame, "EEC1", wire, sizeof(wire), &consumed) == FRAMEDSL_OK && consumed == 8);
  FramedslFieldId speed;
  double rpm = 0;
  EXPECT(framedsl_find(fixture.definition, "EEC1.engine_speed", &speed));
  EXPECT(framedsl_get_eng(fixture.frame, speed, &rpm) == FRAMEDSL_OK && rpm == 621.0);

  const Expected fields[] = {
    { "EEC1.torque_mode",       0xF    },
    { "EEC1.additional_torque", 0xF    },
    { "EEC1.demand_percent",    0xFF   },
    { "EEC1.actual_percent",    0xFF   },
    { "EEC1.engine_speed",      0x1368 },
    { "EEC1.source_address",    0xFF   },
    { "EEC1.starter_mode",      0xF    },
    { "EEC1.reserved",          0xF    },
    { "EEC1.engine_demand",     0xFF   },
  };
  const int result = check_frame(&fixture, "EEC1", wire, sizeof(wire), fields, sizeof(fields) / sizeof(*fields));
  close_example(&fixture);
  return failure != 0 ? failure : result;
}

/* Built from the published layout, as no frame with its fields decoded was found: identity 0x12345 and
 * manufacturer 0x2AB, with fields crossing byte boundaries. */
static int us34_tc04_a_name_from_can(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_example(&fixture, "examples/j1939.fdsl")) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0x45, 0x23, 0x61, 0x55, 0x8D, 0x81, 0x46, 0xA9 };
  EXPECT(framedsl_decode_as(fixture.frame, "ADDRESS_CLAIMED", wire, sizeof(wire), NULL) == FRAMEDSL_OK);
  const Expected fields[] = {
    { "ADDRESS_CLAIMED.identity",          0x12345 },
    { "ADDRESS_CLAIMED.manufacturer",      0x2AB   },
    { "ADDRESS_CLAIMED.ecu_instance",      5       },
    { "ADDRESS_CLAIMED.function_instance", 0x11    },
    { "ADDRESS_CLAIMED.function",          0x81    },
    { "ADDRESS_CLAIMED.reserved",          0       },
    { "ADDRESS_CLAIMED.vehicle_system",    0x23    },
    { "ADDRESS_CLAIMED.system_instance",   9       },
    { "ADDRESS_CLAIMED.industry_group",    2       },
    { "ADDRESS_CLAIMED.arbitrary",         1       },
  };
  const int result =
    check_frame(&fixture, "ADDRESS_CLAIMED", wire, sizeof(wire), fields, sizeof(fields) / sizeof(*fields));
  close_example(&fixture);
  return failure != 0 ? failure : result;
}

/* A wrong checksum is a value, and goes through: it is the program that says so. */
static int us34_tc05_a_wrong_checksum_is_read_not_refused(const TestArguments *args) {
  (void)args;
  Fixture fixture;
  if (!open_example(&fixture, "examples/ublox_nav_posllh.fdsl")) {
    return __LINE__;
  }
  int failure = 0;
  const uint8_t wire[] = { 0xB5, 0x62, 0x01, 0x02, 0x1C, 0x00, 0xA0, 0x86, 0x01, 0x00, 0x00, 0xDE,
                           0x30, 0xDD, 0x80, 0x75, 0x60, 0xEB, 0xA8, 0x61, 0x00, 0x00, 0xE0, 0x2E,
                           0x00, 0x00, 0xDC, 0x05, 0x00, 0x00, 0xC4, 0x09, 0x00, 0x00, 0x00, 0x00 };
  EXPECT(framedsl_decode(fixture.frame, wire, sizeof(wire), NULL, NULL) != NULL);
  EXPECT(reads(&fixture, "NAV_POSLLH.ck_a", 0));
  close_example(&fixture);
  return failure;
}

void register_user_story34(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us34_tc01_each_definition_is_clean, "each definition passes --check --werror", false);
  ADD_TEST_CASE(us34_tc02_a_position_from_a_serial_link, "a u-blox position reads and encodes byte for byte", false);
  ADD_TEST_CASE(us34_tc03_an_engine_speed_from_can, "a J1939 EEC1 reads 621 rpm and encodes byte for byte", false);
  ADD_TEST_CASE(us34_tc04_a_name_from_can, "a J1939 NAME reads and encodes byte for byte", false);
  ADD_TEST_CASE(us34_tc05_a_wrong_checksum_is_read_not_refused, "a wrong checksum is read, not refused", false);
}
