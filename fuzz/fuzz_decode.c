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
#include <stdbool.h>
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

  /* Conditional fields (#12): on a named use, a pasted one, an anonymous block and a field; nested,
   * compared with an enum name and with `!=`; a counted payload and a repetition under one; a field of
   * an instance named through it; and a condition on a field fixed by an override, which must read the
   * fixed value at both ends. An absent field must read as absent, and the frame re-encode exactly. */
  "enum K: 4 { NONE; ONE; TWO; };\n"
  "block Opt { kind: K; flags: 4; [ @if: valof(kind) == ONE; ] x: 8;\n"
  "  [ @if: valof(flags) != 0; ] block { n: 8; [ @if: valof(n) == 3; @byte_order: little_endian; ] y: 16; }; };\n"
  "block H { type: 8; f: 8; [ @if: valof(type) == 0x41; ] ext: 8; };\n"
  "block Pt { a: 8; [ @bit_order: lsb_first; ] b: 8; };\n"
  "pkt C1 { id: 8 = 0x40; flag: 1; pad: 7; [ @if: valof(flag) == 1; ] o: Opt; [ @if: valof(flag) != 1; ] Pt;\n"
  "  len: 8; [ @if: valof(len) != 0; ] d[32]: valof(len) * 8; };\n"
  "pkt C2 { h: H { type = 0x41; }; c: 8; [ @if: valof(h.f) == 1; ] pts[16]: Pt[valof(c)];\n"
  "  [ @if: valof(h.f) == 2; @byte_order: little_endian; ] w: 32; tail: 8; };\n",

  /* Combined conditions (#126): `&&` and `||` with parentheses, orderings, masks, and an enum compared
   * by name, on a field, an anonymous block and a field nested in it. */
  "enum K2: 2 { IDLE; HK; EVENT; DUMP; };\n"
  "pkt CC { id: 8 = 0x7E; version: 4; flags: 4; kind: K2; spare: 6;\n"
  "  [ @if: valof(flags) & 0x1 != 0; ] seq: 16;\n"
  "  [ @if: valof(version) >= 2 && valof(version) < 5; ] ext: 8;\n"
  "  [ @if: valof(kind) == HK || valof(kind) == EVENT; ] block { n: 8;\n"
  "    [ @if: valof(flags) & 0x2 != 0 || valof(version) > 10; ] code: 8; };\n"
  "  [ @if: (valof(flags) & 0x8 == 0x8 || valof(version) == 0) && valof(kind) != IDLE; ] crc: 16;\n"
  "  tail: 8; };\n",

  /* Fixed values past the known prefix (#141): they identify nothing, so a frame may differ from them,
   * and what it answers has to follow what arrived. */
  "block S { v: 8; };\n"
  "pkt FP { id: 8 = 0x71; size: 8; pad[4]: valof(size) * 8; flag: 8 = 1;\n"
  "  [ @if: valof(flag) == 1; ] extra: 8; count: 8 = 2; list[8]: S[valof(count)]; end: 8 = 0x7F; };\n",
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
 * second frame and encodes that. The result must be the bytes the decode consumed, unless a fixed field
 * differs from the definition, which encode does not reproduce. Either way the fields the frame says it
 * holds must add up to those bytes. */
static void check_round_trip(size_t d, const char *name, const uint8_t *bytes, size_t consumed) {
  const FramedslDefinition *definition = framedsl_definition(results[d]);
  size_t packet = 0;
  if (!framedsl_find_packet(definition, name, &packet)) {
    abort();
  }

  uint64_t carried = 0;
  bool as_defined = true;

  /* The length fields go in before the payloads they size, since setting a payload does not care, but
   * the order the fields are declared in already guarantees that. */
  for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
    FramedslField field;
    FramedslFieldId id;
    if (!framedsl_field(definition, packet, f, &field) || !framedsl_field_id(definition, packet, f, &id)) {
      abort();
    }
    /* A field the frame did not carry (#12) reads as absent through every getter, and is not copied:
     * the copy's conditions, made of values copied above, leave it out of the encoding too. */
    bool present = false;
    if (framedsl_present(frames[d], id, &present) != FRAMEDSL_OK || (field.condition == NULL && !present)) {
      abort();
    }
    if (!present) {
      uint64_t value = 0;
      size_t length = 0;
      FramedslStatus status = FRAMEDSL_OK;
      if (field.is_scalar
            ? framedsl_get_u64(frames[d], id, &value) != FRAMEDSL_ERROR_ABSENT
            : framedsl_get_bytes(frames[d], id, &length, &status) != NULL || status != FRAMEDSL_ERROR_ABSENT) {
        abort();
      }
      continue;
    }
    if (field.is_scalar) {
      uint64_t value = 0;
      if (framedsl_get_u64(frames[d], id, &value) != FRAMEDSL_OK) {
        abort();
      }
      carried += field.width_bits;
      as_defined = as_defined && (!field.has_fixed || value == field.fixed);
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
      carried += field.variable ? (uint64_t)length * 8 : field.width_bits;
      size_t count = 0;
      if (field.element_bits != 0
          && (framedsl_count(frames[d], id, &count) != FRAMEDSL_OK
              || (uint64_t)count * field.element_bits != (field.variable ? (uint64_t)length * 8 : field.width_bits))) {
        abort();
      }
    }
  }
  if (carried != (uint64_t)consumed * 8) {
    abort();
  }
  if (!as_defined) {
    return;
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
