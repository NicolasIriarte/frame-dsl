/* Story 5: turning a frame's values into bytes.
 *
 * Every expectation here is a hand-written byte sequence, worked out from the definition rather than
 * from the encoder. That is the whole discipline of this file: there is no decoder yet, and when
 * there is one, a round trip will not be evidence. Two matching bugs cancel, and the frames this
 * library exists to produce are defined by somebody else's specification, not by its own reader. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocations.h"
#include "framedsl.h"
#include "testsuite.h"

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(const char *source, Fixture *fixture) {
  fixture->result = framedsl_load_string(source, "encode.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = framedsl_frame_new(fixture->definition);
  return framedsl_ok(fixture->result) && fixture->frame != NULL;
}

static void close_fixture(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static bool set_scalar(Fixture *fixture, const char *dotted, uint64_t value) {
  FramedslFieldId id;
  return framedsl_find(fixture->definition, dotted, &id) &&
         framedsl_set_u64(fixture->frame, id, value) == FRAMEDSL_OK;
}

/* Encodes and compares against `expected`, reporting where the two first differ. */
static int expect_bytes(Fixture *fixture, const char *packet, const uint8_t *expected,
                        size_t expected_length, int line) {
  size_t length = 0;
  FramedslStatus status = FRAMEDSL_OK;
  const uint8_t *bytes = framedsl_encode(fixture->frame, packet, &length, &status);

  if (bytes == NULL || status != FRAMEDSL_OK) {
    return line;
  }
  if (length != expected_length || memcmp(bytes, expected, expected_length) != 0) {
    return line;
  }
  return 0;
}

/* A packet of whole-byte fields, big-endian and msb_first: the bytes are the values, in order. */
static int us5_tc01_a_byte_aligned_packet_encodes_plainly(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { hdr: 8 = 0xAA; seq: 16; tail: 8; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t expected[] = {0xAA, 0x12, 0x34, 0x5F};
  int failure = 0;

  if (!set_scalar(&fixture, "A.seq", 0x1234) || !set_scalar(&fixture, "A.tail", 0x5F)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

/* Sub-byte fields pack against each other with no padding, and a field straddling a byte boundary is
 * split across the two.
 *
 * version 3 = 0b000, type 1 = 0, sec 1 = 1, apid 11 = 0b101_0101_0101:
 *   bits:  000 0 1 10101010101
 *   bytes: 0000 1101  0101 0101  ->  0x0D 0x55
 * then seq_flags 2 = 0b11, seq_count 14 = 0x0001:
 *   bits:  11 00000000000001    ->  1100 0000  0000 0001  ->  0xC0 0x01 */
static int us5_tc02_sub_byte_fields_pack_and_straddle(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { version: 3 = 0b000; type: 1 = 0b0; sec: 1; apid: 11;"
                    " seq_flags: 2; seq_count: 14; };",
                    &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t expected[] = {0x0D, 0x55, 0xC0, 0x01};
  int failure = 0;

  if (!set_scalar(&fixture, "A.sec", 1) || !set_scalar(&fixture, "A.apid", 0x555) ||
      !set_scalar(&fixture, "A.seq_flags", 3) || !set_scalar(&fixture, "A.seq_count", 1)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

/* The same value, the same width, side by side, laid out two ways. `a` is 0x0F msb_first, so its
 * high nibble leads: 0000 1111. `b` is 0x0F lsb_first, which reverses the bits within the field:
 * 1111 0000. Together, 0x0F then 0xF0. */
static int us5_tc03_bit_order_changes_the_bytes(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { h: 8 = 0xAA; a: 8; [ @bit_order: lsb_first; ] b: 8; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t expected[] = {0xAA, 0x0F, 0xF0};
  int failure = 0;

  if (!set_scalar(&fixture, "A.a", 0x0F) || !set_scalar(&fixture, "A.b", 0x0F)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

/* byte_order reorders whole bytes and leaves its neighbours alone. 0x11223344 goes out big-endian as
 * 11 22 33 44 and little-endian as 44 33 22 11; the 0xBB on either side does not move. */
static int us5_tc04_byte_order_swaps_only_its_own_field(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { h: 8 = 0xBB; big: 32;"
                    " [ @byte_order: little_endian; ] little: 32; t: 8 = 0xBB; };",
                    &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t expected[] = {0xBB, 0x11, 0x22, 0x33, 0x44, 0x44, 0x33, 0x22, 0x11, 0xBB};
  int failure = 0;

  if (!set_scalar(&fixture, "A.big", 0x11223344) || !set_scalar(&fixture, "A.little", 0x11223344)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

/* Whatever the caller put in the slot, the frame identifies itself. */
static int us5_tc05_a_fixed_field_comes_from_the_definition(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { hdr: 8 = 0xAA; body: 8; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t expected[] = {0xAA, 0x01};
  int failure = 0;

  /* Deliberately writing something else over the discriminator first. */
  if (!set_scalar(&fixture, "A.hdr", 0x55) || !set_scalar(&fixture, "A.body", 1)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

#define VARIABLE_DEFINITION "pkt A { hdr: 8 = 0xAA; len: 8; data[8]: valof(len) * 8; crc: 8; };"

static int us5_tc06_a_variable_field_encodes_at_every_length(const TestArguments *args) {
  (void)args;

  const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  /* Zero, a middling length, and exactly the reservation. */
  const size_t lengths[] = {0, 3, 8};
  int failure = 0;

  for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths) && failure == 0; ++i) {
    Fixture fixture;
    if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
      close_fixture(&fixture);
      FORCE_FAILURE();
    }

    uint8_t expected[16] = {0xAA, (uint8_t)lengths[i]};
    memcpy(expected + 2, payload, lengths[i]);
    expected[2 + lengths[i]] = 0xC7;

    FramedslFieldId data;
    if (!framedsl_find(fixture.definition, "A.data", &data) ||
        !set_scalar(&fixture, "A.len", lengths[i]) ||
        framedsl_set_bytes(fixture.frame, data, payload, lengths[i]) != FRAMEDSL_OK ||
        !set_scalar(&fixture, "A.crc", 0xC7)) {
      failure = __LINE__;
    } else {
      failure = expect_bytes(&fixture, "A", expected, 3 + lengths[i], __LINE__);
    }

    close_fixture(&fixture);
  }
  return failure;
}

/* The length field and the bytes describe two different frames, and the definition is what decides.
 * Truncating silently would send a frame whose own length field contradicts it. */
static int us5_tc07_a_length_disagreeing_with_the_bytes_is_refused(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t payload[4] = {1, 2, 3, 4};
  FramedslFieldId data;
  size_t length = 0;
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.data", &data) ||
      framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK ||
      !set_scalar(&fixture, "A.len", 2)) {
    failure = __LINE__;
  } else if (framedsl_encode(fixture.frame, "A", &length, &status) != NULL) {
    failure = __LINE__;
  } else if (status != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A length field implying more than the field reserved. The arena is never overrun; the encode
 * simply stops. */
static int us5_tc08_a_length_over_the_reservation_is_refused(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  size_t length = 0;
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  if (!set_scalar(&fixture, "A.len", 200)) {
    failure = __LINE__;
  } else if (framedsl_encode(fixture.frame, "A", &length, &status) != NULL ||
             status != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

static int us5_tc09_an_unknown_packet_is_an_error(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { hdr: 8 = 0xAA; body: 8; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  size_t length = 0;
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  if (framedsl_encode(fixture.frame, "NOPE", &length, &status) != NULL ||
      status != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  } else if (framedsl_encode(fixture.frame, NULL, &length, &status) != NULL) {
    failure = __LINE__;
  } else if (framedsl_encode(NULL, "A", &length, &status) != NULL ||
             status != FRAMEDSL_ERROR_MEMORY) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A shorter frame must not leave any of a longer one behind it. Encoding eight bytes of payload and
 * then none of it has to produce a three-byte frame with nothing of the first in it. */
static int us5_tc10_a_later_encode_does_not_show_an_earlier_one(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t payload[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  const uint8_t expected[] = {0xAA, 0x00, 0xC7};
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.data", &data) ||
      framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK ||
      !set_scalar(&fixture, "A.len", 8) || !set_scalar(&fixture, "A.crc", 0xC7)) {
    failure = __LINE__;
  } else {
    size_t length = 0;
    if (framedsl_encode(fixture.frame, "A", &length, NULL) == NULL || length != 11) {
      failure = __LINE__;
    } else if (framedsl_set_bytes(fixture.frame, data, payload, 0) != FRAMEDSL_OK ||
               !set_scalar(&fixture, "A.len", 0)) {
      failure = __LINE__;
    } else {
      failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* Encoding twice gives the same answer, and a bit string goes out as the bytes it was handed. */
static int us5_tc11_a_bit_string_passes_through_unchanged(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { h: 8 = 0xAA; wide: 72; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t wide_bytes[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  const uint8_t expected[] = {0xAA, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  FramedslFieldId wide;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.wide", &wide) ||
      framedsl_set_bytes(fixture.frame, wide, wide_bytes, sizeof(wide_bytes)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
    if (failure == 0) {
      failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* A bit string that does not start on a byte boundary is shifted across the bytes it spans, which is
 * the case a memcpy would get wrong. Four bits of 0xA, then nine bytes: each output byte carries the
 * low nibble of one input byte and the high nibble of the next. */
static int us5_tc12_a_bit_string_off_a_byte_boundary_is_shifted(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { h: 4 = 0xA; wide: 72; pad: 4; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t wide_bytes[9] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11};
  const uint8_t expected[] = {0xA1, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x15};
  FramedslFieldId wide;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.wide", &wide) ||
      framedsl_set_bytes(fixture.frame, wide, wide_bytes, sizeof(wide_bytes)) != FRAMEDSL_OK ||
      !set_scalar(&fixture, "A.pad", 0x5)) {
    failure = __LINE__;
  } else {
    failure = expect_bytes(&fixture, "A", expected, sizeof(expected), __LINE__);
  }

  close_fixture(&fixture);
  return failure;
}

/* The claim the arena exists for, and the one an encoder is most likely to break. */
static int us5_tc13_encoding_does_not_allocate(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t payload[4] = {1, 2, 3, 4};
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.data", &data) ||
      framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK ||
      !set_scalar(&fixture, "A.len", 4)) {
    failure = __LINE__;
  } else {
    size_t length = 0;
    framedsl_allocation_count_start();
    for (int i = 0; i < 64; ++i) {
      framedsl_encode(fixture.frame, "A", &length, NULL);
    }
    if (framedsl_allocation_count_stop() != 0) {
      failure = __LINE__;
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* ---- Into a buffer the caller owns -------------------------------------------------------------
 *
 * framedsl_encode() lends a pointer valid until the next call. A caller who already has somewhere the
 * frame has to end up -- a transmit ring, a region of a larger message -- needs the other direction,
 * and needs to be told when their buffer is too small rather than finding out afterwards. */

/* The buffer is allocated at exactly the size needed, so a write past the frame is a write past the
 * allocation and the sanitiser says so rather than it going unnoticed in the slack of an array. */
static int us5_tc14_encoding_into_a_caller_buffer_writes_the_same_bytes(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(VARIABLE_DEFINITION, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t payload[4] = {1, 2, 3, 4};
  const uint8_t expected[] = {0xAA, 0x04, 1, 2, 3, 4, 0xC7};
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "A.data", &data) ||
      framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK ||
      !set_scalar(&fixture, "A.len", 4) || !set_scalar(&fixture, "A.crc", 0xC7)) {
    failure = __LINE__;
  } else {
    uint8_t *exact = malloc(sizeof(expected));
    size_t length = 0;

    if (exact == NULL) {
      failure = __LINE__;
    } else if (framedsl_encode_into(fixture.frame, "A", exact, sizeof(expected), &length) !=
               FRAMEDSL_OK) {
      failure = __LINE__;
    } else if (length != sizeof(expected) || memcmp(exact, expected, sizeof(expected)) != 0) {
      failure = __LINE__;
    }
    free(exact);

    /* A buffer with room to spare takes the frame and nothing else: the bytes past it are the
     * caller's, and an encoder that cleared them would be corrupting a message it was handed a
     * window into. */
    if (failure == 0) {
      uint8_t roomy[32];
      memset(roomy, 0xEE, sizeof(roomy));

      if (framedsl_encode_into(fixture.frame, "A", roomy, sizeof(roomy), &length) != FRAMEDSL_OK) {
        failure = __LINE__;
      } else if (length != sizeof(expected) || memcmp(roomy, expected, sizeof(expected)) != 0) {
        failure = __LINE__;
      } else {
        for (size_t i = sizeof(expected); i < sizeof(roomy); ++i) {
          if (roomy[i] != 0xEE) {
            failure = __LINE__;
            break;
          }
        }
      }
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* One byte short is refused, says how many were needed, and leaves the buffer alone -- a partial
 * frame in a caller's ring would be sent as though it were whole. */
static int us5_tc15_a_buffer_too_small_is_refused_with_the_size_needed(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture("pkt A { hdr: 8 = 0xAA; seq: 16; tail: 8; };", &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  uint8_t buffer[4];
  memset(buffer, 0xEE, sizeof(buffer));
  size_t length = 0;
  int failure = 0;

  if (!set_scalar(&fixture, "A.seq", 0x1234) || !set_scalar(&fixture, "A.tail", 0x5F)) {
    failure = __LINE__;
  } else if (framedsl_encode_into(fixture.frame, "A", buffer, 3, &length) != FRAMEDSL_ERROR_BUFFER) {
    failure = __LINE__;
  } else if (length != 4) {
    failure = __LINE__; /* the size needed, so the next attempt is sized from the answer */
  } else if (buffer[0] != 0xEE || buffer[1] != 0xEE || buffer[2] != 0xEE) {
    failure = __LINE__; /* not a partial frame, not the bytes that would have fit */
  } else if (framedsl_encode_into(fixture.frame, "A", NULL, 0, &length) != FRAMEDSL_ERROR_BUFFER ||
             length != 4) {
    failure = __LINE__; /* asking how big it would be, without a buffer to write it to */
  } else if (framedsl_encode_into(fixture.frame, "A", buffer, sizeof(buffer), &length) !=
                 FRAMEDSL_OK ||
             length != 4) {
    failure = __LINE__; /* and then it fits, at exactly the size just reported */
  } else if (framedsl_encode_into(fixture.frame, "NOPE", buffer, sizeof(buffer), &length) !=
             FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* every other refusal is the one framedsl_encode() gives */
  }

  close_fixture(&fixture);
  return failure;
}

void register_user_story5(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us5_tc01_a_byte_aligned_packet_encodes_plainly,
                "whole-byte fields encode to their values in order", false);
  ADD_TEST_CASE(us5_tc02_sub_byte_fields_pack_and_straddle,
                "sub-byte fields pack with no padding and cross byte boundaries", false);
  ADD_TEST_CASE(us5_tc03_bit_order_changes_the_bytes,
                "lsb_first and msb_first lay the same value out differently", false);
  ADD_TEST_CASE(us5_tc04_byte_order_swaps_only_its_own_field,
                "little_endian swaps its field's bytes and no others", false);
  ADD_TEST_CASE(us5_tc05_a_fixed_field_comes_from_the_definition,
                "a fixed field is written from the definition, not the arena", false);
  ADD_TEST_CASE(us5_tc06_a_variable_field_encodes_at_every_length,
                "a valof() field encodes at zero, a middling length and its reservation", false);
  ADD_TEST_CASE(us5_tc07_a_length_disagreeing_with_the_bytes_is_refused,
                "a length field contradicting the bytes is an error", false);
  ADD_TEST_CASE(us5_tc08_a_length_over_the_reservation_is_refused,
                "a length past the reservation is an error", false);
  ADD_TEST_CASE(us5_tc09_an_unknown_packet_is_an_error, "an unknown packet name is an error", false);
  ADD_TEST_CASE(us5_tc10_a_later_encode_does_not_show_an_earlier_one,
                "a shorter frame leaves nothing of a longer one behind", false);
  ADD_TEST_CASE(us5_tc11_a_bit_string_passes_through_unchanged,
                "a bit string goes out as the bytes it was given", false);
  ADD_TEST_CASE(us5_tc12_a_bit_string_off_a_byte_boundary_is_shifted,
                "a bit string not starting on a byte boundary is shifted", false);
  ADD_TEST_CASE(us5_tc13_encoding_does_not_allocate, "encoding allocates nothing, counted", false);
  ADD_TEST_CASE(us5_tc14_encoding_into_a_caller_buffer_writes_the_same_bytes,
                "encoding into a caller's buffer writes the frame and nothing past it", false);
  ADD_TEST_CASE(us5_tc15_a_buffer_too_small_is_refused_with_the_size_needed,
                "too small a buffer is refused, untouched, with the size needed", false);
}
