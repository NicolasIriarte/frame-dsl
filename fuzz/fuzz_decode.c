/* Fuzz target: arbitrary bytes off a link, decoded against definitions that cover every shape the
 * decoder walks.
 *
 * Crashes and sanitizer reports are the obvious findings. The less obvious one is a wrong answer, so
 * every frame that decodes is encoded again from what the decode left in the frame, and must come
 * back as exactly the bytes it was decoded from: a decoder and an encoder that disagree about a
 * layout are a bug even when neither crashes.
 *
 * Built with libFuzzer by `make fuzz`, and replayed over fuzz/corpus/ and fuzz/regressions/ by a
 * plain driver in `make test` (fuzz/replay.c). Against the public header alone. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"

static const char *const DEFINITIONS[] = {
  /* Whole bytes, a valof() payload counted in bytes, and three distinguishable packets. */
  "pkt A { id: 8 = 0x01; seq: 16; };\n"
  "pkt B { id: 8 = 0x02; seq: 16; mode: 8; };\n"
  "pkt C { id: 8 = 0x03; seq: 16; len: 16; body[64]: valof(len) * 8; };\n",

  /* Sub-byte fields crossing byte boundaries, a length counting itself, lsb_first and little_endian. */
  "pkt S {\n"
  "  version: 3 = 0b000; type: 1 = 0b1; flag: 1; apid: 11; seq_flags: 2; count: 14;\n"
  "  data_length: 16;\n"
  "  user[128]: (valof(data_length) + 1) * 8;\n"
  "};\n"
  "[ @bit_order: lsb_first; ] pkt L { id: 8 = 0x10; a: 3; b: 13; [ @byte_order: little_endian; ] c: 32; };\n",

  /* A bit string at an odd offset, a 64-bit scalar, composition and calibrations. */
  "cal volts(raw) = raw * 3.3 / 4095;\n"
  "cal ohms(raw) = 10000 * raw / (4095 - raw) inverse (eng) = 4095 * eng / (10000 + eng);\n"
  "block H { id: 8; seq: 16; };\n"
  "pkt W { h: H { id = 0x20; }; pad: 5; blob: 72; big: 64; tail: 3; };\n"
  "pkt X { h: H { id = 0x21; }; [ @cal: volts; ] v: 12; [ @cal: ohms; ] r: 12; n: 8; d[16]: valof(n) * 8; };\n",

  /* Enums (#11): a discriminator fixed by name, sub-byte and little-endian fields, and names that leave
   * most values unlisted -- which must decode, and read back as unlisted rather than as anything. */
  "enum T: 4 { HK = 0x1; EVENT; STATUS = 0x8; };\n"
  "enum Level: 3 { OFF; LOW; MAX = 7; };\n"
  "enum Mode: 16 { IDLE = 0x0100; RUN; };\n"
  "block Hdr { type: T; seq: 12; };\n"
  "pkt E1 { h: Hdr { type = HK; }; level: Level; pad: 5; [ @byte_order: little_endian; ] mode: Mode; };\n"
  "pkt E2 { h: Hdr { type = EVENT; }; mode: Mode = RUN; };\n",
};

#define DEFINITION_COUNT (sizeof(DEFINITIONS) / sizeof(*DEFINITIONS))

static FramedslResult *results[DEFINITION_COUNT];
static FramedslFrame *frames[DEFINITION_COUNT];
static FramedslFrame *checks[DEFINITION_COUNT];

static void load_once(void) {
  static int loaded = 0;
  if (loaded) {
    return;
  }
  loaded = 1;
  for (size_t i = 0; i < DEFINITION_COUNT; ++i) {
    results[i] = framedsl_load_string(DEFINITIONS[i], "fuzz.fdsl");
    if (!framedsl_ok(results[i])) {
      abort(); /* a definition here that stops loading is a broken target, not a finding */
    }
    frames[i] = framedsl_frame_new(framedsl_definition(results[i]));
    checks[i] = framedsl_frame_new(framedsl_definition(results[i]));
  }
}

/* Reads every field of the decoded packet, the way a caller would, then copies the values into a
 * second frame and encodes that. The result must be the bytes the decode consumed. */
static void check_round_trip(size_t d, const char *name, const uint8_t *bytes, size_t consumed) {
  const FramedslDefinition *definition = framedsl_definition(results[d]);
  size_t packet = 0;
  if (!framedsl_find_packet(definition, name, &packet)) {
    abort();
  }

  /* The length fields go in before the payloads they size, since setting a payload does not care, but
   * the order the fields are declared in already guarantees that. */
  for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
    FramedslField field;
    FramedslFieldId id;
    if (!framedsl_field(definition, packet, f, &field) || !framedsl_field_id(definition, packet, f, &id)) {
      abort();
    }
    if (field.is_scalar) {
      uint64_t value = 0;
      if (framedsl_get_u64(frames[d], id, &value) != FRAMEDSL_OK) {
        abort();
      }
      double eng;
      if (field.cal != NULL) {
        (void)framedsl_get_eng(frames[d], id, &eng); /* may legitimately be RANGE; must not crash */
      }
      const char *named = NULL;
      if (field.enumeration != NULL) {
        FramedslStatus status = framedsl_get_name(frames[d], id, &named);
        if ((status == FRAMEDSL_OK) != (named != NULL)
            || (status != FRAMEDSL_OK && status != FRAMEDSL_ERROR_UNLISTED)) {
          abort();
        }
        if (named != NULL && !field.has_fixed && framedsl_set_name(checks[d], id, named) != FRAMEDSL_OK) {
          abort(); /* a name read back sets the same value; set_u64 below then writes it again */
        }
      }
      if (!field.has_fixed && framedsl_set_u64(checks[d], id, value) != FRAMEDSL_OK) {
        abort();
      }
    } else {
      size_t length = 0;
      const uint8_t *data = framedsl_get_bytes(frames[d], id, &length, NULL);
      if (data == NULL || framedsl_set_bytes(checks[d], id, data, length) != FRAMEDSL_OK) {
        abort();
      }
    }
  }

  size_t length = 0;
  const uint8_t *encoded = framedsl_encode(checks[d], name, &length, NULL);
  if (encoded == NULL || length != consumed || memcmp(encoded, bytes, length) != 0) {
    abort();
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  load_once();

  for (size_t d = 0; d < DEFINITION_COUNT; ++d) {
    /* A capture: frames back to back, walked with `consumed` until one fails to decode. */
    size_t offset = 0;
    while (offset < size) {
      size_t consumed = 0;
      FramedslStatus status = FRAMEDSL_OK;
      const char *name = framedsl_decode(frames[d], data + offset, size - offset, &consumed, &status);
      if (name == NULL) {
        if (status != FRAMEDSL_ERROR_UNRECOGNISED && status != FRAMEDSL_ERROR_TRUNCATED
            && status != FRAMEDSL_ERROR_RANGE) {
          abort(); /* the only three answers a decode may give */
        }
        break;
      }
      if (consumed == 0 || consumed > size - offset) {
        abort();
      }
      check_round_trip(d, name, data + offset, consumed);
      offset += consumed;
    }
  }
  return 0;
}
