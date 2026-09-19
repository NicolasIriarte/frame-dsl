/* Story 6: reading a frame back, and the end-to-end milestone.
 *
 * Values are checked against hand-written bytes first, for the same reason story 5 does it: a round
 * trip is not evidence, because two matching bugs cancel. The round trip is tested too, at the end,
 * because it is the thing the project set out to do -- but it is the milestone, not the proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocations.h"
#include "framedsl.h"
#include "testsuite.h"

/* Three packets that pin the same eight bits to different values, plus a variable-length one. */
#define FAMILY                                                                                                         \
  "@bit_order:  msb_first;\n"                                                                                          \
  "@byte_order: big_endian;\n"                                                                                         \
  "pkt TC_PING     { type: 8 = 0x01; seq: 16; };\n"                                                                    \
  "pkt TC_SET_MODE { type: 8 = 0x02; seq: 16; mode: 8; };\n"                                                           \
  "pkt TC_PAYLOAD  { type: 8 = 0x03; len: 8; body[8]: valof(len) * 8; crc: 8; };\n"

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool open_fixture(const char *source, Fixture *fixture) {
  fixture->result = framedsl_load_string(source, "decode.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = framedsl_frame_new(fixture->definition);
  return framedsl_ok(fixture->result) && fixture->frame != NULL;
}

static void close_fixture(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

static bool scalar_is(Fixture *fixture, const char *dotted, uint64_t expected) {
  FramedslFieldId id;
  uint64_t value = 0;
  return framedsl_find(fixture->definition, dotted, &id) && framedsl_get_u64(fixture->frame, id, &value) == FRAMEDSL_OK
         && value == expected;
}

/* Hand-written bytes in, values out. Nothing here has been through the encoder. */
static int us6_tc01_hand_written_bytes_decode_to_their_values(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t bytes[] = { 0x02, 0x12, 0x34, 0x7F };
  size_t consumed = 0;
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  const char *packet = framedsl_decode(fixture.frame, bytes, sizeof(bytes), &consumed, &status);

  if (packet == NULL || strcmp(packet, "TC_SET_MODE") != 0) {
    failure = __LINE__;
  } else if (consumed != 4) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "TC_SET_MODE.seq", 0x1234)) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "TC_SET_MODE.mode", 0x7F)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* Three packets pinning the same bits to different values: each frame finds its own. */
static int us6_tc02_the_right_packet_is_identified(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t ping[] = { 0x01, 0x00, 0x09 };
  const uint8_t set_mode[] = { 0x02, 0x00, 0x09, 0x01 };
  const uint8_t payload[] = { 0x03, 0x01, 0xEE, 0xC7 };
  int failure = 0;

  const char *first = framedsl_decode(fixture.frame, ping, sizeof(ping), NULL, NULL);
  if (first == NULL || strcmp(first, "TC_PING") != 0) {
    failure = __LINE__;
  } else {
    const char *second = framedsl_decode(fixture.frame, set_mode, sizeof(set_mode), NULL, NULL);
    if (second == NULL || strcmp(second, "TC_SET_MODE") != 0) {
      failure = __LINE__;
    } else {
      const char *third = framedsl_decode(fixture.frame, payload, sizeof(payload), NULL, NULL);
      if (third == NULL || strcmp(third, "TC_PAYLOAD") != 0) {
        failure = __LINE__;
      }
    }
  }

  close_fixture(&fixture);
  return failure;
}

/* Three failures that mean three different things to the program handling them. */
static int us6_tc03_the_failures_are_told_apart(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t noise[] = { 0xFF, 0x00, 0x00, 0x00 };
  const uint8_t cut_short[] = { 0x02, 0x12 };
  const uint8_t over_bound[] = { 0x03, 0xFF, 0x00, 0x00 };
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  if (framedsl_decode(fixture.frame, noise, sizeof(noise), NULL, &status) != NULL
      || status != FRAMEDSL_ERROR_UNRECOGNISED) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, cut_short, sizeof(cut_short), NULL, &status) != NULL
             || status != FRAMEDSL_ERROR_TRUNCATED) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, over_bound, sizeof(over_bound), NULL, &status) != NULL
             || status != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__; /* len = 255, and body reserved eight bytes */
  }

  close_fixture(&fixture);
  return failure;
}

/* A buffer sized exactly, so anything read past the frame is read past the allocation and the
 * sanitiser says so. */
static int us6_tc04_nothing_is_read_past_the_buffer(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  int failure = 0;
  uint8_t *exact = malloc(3);
  if (exact == NULL) {
    failure = __LINE__;
  } else {
    exact[0] = 0x01;
    exact[1] = 0xAB;
    exact[2] = 0xCD;

    const char *packet = framedsl_decode(fixture.frame, exact, 3, NULL, NULL);
    if (packet == NULL || strcmp(packet, "TC_PING") != 0) {
      failure = __LINE__;
    } else if (!scalar_is(&fixture, "TC_PING.seq", 0xABCD)) {
      failure = __LINE__;
    }
    free(exact);
  }

  close_fixture(&fixture);
  return failure;
}

static int us6_tc05_a_variable_field_decodes_at_every_length(const TestArguments *args) {
  (void)args;

  const size_t lengths[] = { 0, 3, 8 };
  int failure = 0;

  for (size_t i = 0; i < sizeof(lengths) / sizeof(*lengths) && failure == 0; ++i) {
    Fixture fixture;
    if (!open_fixture(FAMILY, &fixture)) {
      close_fixture(&fixture);
      FORCE_FAILURE();
    }

    uint8_t bytes[16] = { 0x03, (uint8_t)lengths[i] };
    for (size_t b = 0; b < lengths[i]; ++b) {
      bytes[2 + b] = (uint8_t)(0xA0 + b);
    }
    bytes[2 + lengths[i]] = 0xC7;

    size_t consumed = 0;
    const char *packet = framedsl_decode(fixture.frame, bytes, 3 + lengths[i], &consumed, NULL);

    FramedslFieldId body;
    uint8_t read[16] = { 0 };
    size_t read_length = 0;

    if (packet == NULL || strcmp(packet, "TC_PAYLOAD") != 0) {
      failure = __LINE__;
    } else if (consumed != 3 + lengths[i]) {
      failure = __LINE__;
    } else if (!framedsl_find(fixture.definition, "TC_PAYLOAD.body", &body)) {
      failure = __LINE__;
    } else if (framedsl_get_bytes_into(fixture.frame, body, read, sizeof(read), &read_length) != FRAMEDSL_OK) {
      failure = __LINE__;
    } else if (read_length != lengths[i] || memcmp(read, bytes + 2, lengths[i]) != 0) {
      failure = __LINE__;
    } else if (!scalar_is(&fixture, "TC_PAYLOAD.crc", 0xC7)) {
      failure = __LINE__; /* the field after a variable one lands where the width put it */
    }

    close_fixture(&fixture);
  }
  return failure;
}

/* The orders, read back from bytes nobody's encoder produced. `a` is 0x0F laid out msb_first and `b`
 * the same value lsb_first, so the bytes are 0x0F and 0xF0 and both must come back as 0x0F. */
static int us6_tc06_bit_and_byte_order_are_undone(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(
        "pkt A { h: 8 = 0xAA; a: 8; [ @bit_order: lsb_first; ] b: 8;"
        " [ @byte_order: little_endian; ] c: 32; };",
        &fixture
      )) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t bytes[] = { 0xAA, 0x0F, 0xF0, 0x44, 0x33, 0x22, 0x11 };
  int failure = 0;

  if (framedsl_decode(fixture.frame, bytes, sizeof(bytes), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "A.a", 0x0F)) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "A.b", 0x0F)) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "A.c", 0x11223344)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* A frame embedded in a stream: decode, advance by what it consumed, decode the next. This is what
 * `consumed` is for -- TC_PAYLOAD's length is not knowable without decoding it. */
static int us6_tc07_a_stream_can_be_walked_frame_by_frame(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t stream[] = {
    0x01, 0x00, 0x01, /* TC_PING, 3 bytes */
    0x03, 0x02, 0xAA, 0xBB, 0xC7, /* TC_PAYLOAD with two bytes of body, 5 bytes */
    0x02, 0x00, 0x02, 0x09, /* TC_SET_MODE, 4 bytes */
  };
  const char *expected[] = { "TC_PING", "TC_PAYLOAD", "TC_SET_MODE" };
  const size_t sizes[] = { 3, 5, 4 };

  size_t offset = 0;
  int failure = 0;

  for (size_t i = 0; i < 3 && failure == 0; ++i) {
    size_t consumed = 0;
    const char *packet = framedsl_decode(fixture.frame, stream + offset, sizeof(stream) - offset, &consumed, NULL);
    if (packet == NULL || strcmp(packet, expected[i]) != 0) {
      failure = __LINE__;
    } else if (consumed != sizes[i]) {
      failure = __LINE__;
    }
    offset += consumed;
  }

  if (failure == 0 && offset != sizeof(stream)) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

static int us6_tc08_decoding_does_not_allocate(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t bytes[] = { 0x03, 0x02, 0xAA, 0xBB, 0xC7 };
  int failure = 0;

  framedsl_allocation_count_start();
  for (int i = 0; i < 64; ++i) {
    framedsl_decode(fixture.frame, bytes, sizeof(bytes), NULL, NULL);
  }
  if (framedsl_allocation_count_stop() != 0) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

static int us6_tc09_null_is_tolerated(const TestArguments *args) {
  (void)args;

  const uint8_t bytes[] = { 0x01, 0x00, 0x00 };
  FramedslStatus status = FRAMEDSL_OK;

  if (framedsl_decode(NULL, bytes, sizeof(bytes), NULL, &status) != NULL || status != FRAMEDSL_ERROR_MEMORY) {
    FORCE_FAILURE();
  }
  return 0;
}

/* ---- The milestone ---------------------------------------------------------------------------
 *
 * Define a frame, set every field by name, encode it, hand the buffer to something that sends it,
 * decode it on the other side into a second frame, and read every field back.
 *
 * The two frames are separate on purpose. One frame that encoded and then decoded into itself would
 * still hold the values it started with, and the case would pass without the decoder doing anything
 * at all. */

static uint8_t sent[64];
static size_t sent_length;

/* Stands in for a socket. Takes a buffer and a length and knows nothing else -- which is the point
 * of the encoder handing back a plain pointer. */
static void send_it(const uint8_t *bytes, size_t length) {
  memcpy(sent, bytes, length);
  sent_length = length;
}

static int us6_tc10_a_frame_survives_a_round_trip(const TestArguments *args) {
  (void)args;

  Fixture sender = {};
  Fixture receiver = {};
  if (!open_fixture(FAMILY, &sender) || !open_fixture(FAMILY, &receiver)) {
    close_fixture(&sender);
    close_fixture(&receiver);
    FORCE_FAILURE();
  }

  const uint8_t payload[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };
  FramedslFieldId len;
  FramedslFieldId body;
  FramedslFieldId crc;
  int failure = 0;

  if (!framedsl_find(sender.definition, "TC_PAYLOAD.len", &len)
      || !framedsl_find(sender.definition, "TC_PAYLOAD.body", &body)
      || !framedsl_find(sender.definition, "TC_PAYLOAD.crc", &crc)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(sender.frame, len, sizeof(payload)) != FRAMEDSL_OK
             || framedsl_set_bytes(sender.frame, body, payload, sizeof(payload)) != FRAMEDSL_OK
             || framedsl_set_u64(sender.frame, crc, 0x5A) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    size_t length = 0;
    FramedslStatus status = FRAMEDSL_OK;
    const uint8_t *bytes = framedsl_encode(sender.frame, "TC_PAYLOAD", &length, &status);

    if (bytes == NULL) {
      failure = __LINE__;
    } else {
      send_it(bytes, length);

      /* ...and on the other side, with its own frame over the same definition. */
      FramedslFieldId their_len;
      FramedslFieldId their_body;
      FramedslFieldId their_crc;
      uint8_t received[16] = { 0 };
      size_t received_length = 0;
      uint64_t value = 0;

      const char *packet = framedsl_decode(receiver.frame, sent, sent_length, NULL, &status);

      if (packet == NULL || strcmp(packet, "TC_PAYLOAD") != 0) {
        failure = __LINE__;
      } else if (!framedsl_find(receiver.definition, "TC_PAYLOAD.len", &their_len)
                 || !framedsl_find(receiver.definition, "TC_PAYLOAD.body", &their_body)
                 || !framedsl_find(receiver.definition, "TC_PAYLOAD.crc", &their_crc)) {
        failure = __LINE__;
      } else if (framedsl_get_u64(receiver.frame, their_len, &value) != FRAMEDSL_OK || value != sizeof(payload)) {
        failure = __LINE__;
      } else if (framedsl_get_bytes_into(receiver.frame, their_body, received, sizeof(received), &received_length)
                 != FRAMEDSL_OK) {
        failure = __LINE__;
      } else if (received_length != sizeof(payload) || memcmp(received, payload, sizeof(payload)) != 0) {
        failure = __LINE__;
      } else if (framedsl_get_u64(receiver.frame, their_crc, &value) != FRAMEDSL_OK || value != 0x5A) {
        failure = __LINE__;
      }
    }
  }

  close_fixture(&sender);
  close_fixture(&receiver);
  return failure;
}

/* The same, against a definition file on disk rather than a string, because that is how a program
 * actually uses this: the layout is data, loaded at startup, and the program is not rebuilt when it
 * changes. */
static int us6_tc11_the_round_trip_works_from_a_file(const TestArguments *args) {
  (void)args;

  char path[1024];
  snprintf(path, sizeof(path), "%s/tm_datagram.fdsl", EXAMPLES_DIR);

  FramedslResult *result = framedsl_load_file(path);
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *sender = framedsl_frame_new(definition);
  FramedslFrame *receiver = framedsl_frame_new(definition);

  const uint8_t payload[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
  FramedslFieldId len;
  FramedslFieldId data;
  FramedslFieldId crc;
  int failure = 0;

  if (!framedsl_ok(result) || sender == NULL || receiver == NULL) {
    failure = __LINE__;
  } else if (!framedsl_find(definition, "TM_DATAGRAM_02.len", &len)
             || !framedsl_find(definition, "TM_DATAGRAM_02.data", &data)
             || !framedsl_find(definition, "TM_DATAGRAM_02.crc", &crc)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(sender, len, sizeof(payload)) != FRAMEDSL_OK
             || framedsl_set_bytes(sender, data, payload, sizeof(payload)) != FRAMEDSL_OK
             || framedsl_set_u64(sender, crc, 0xBEEF) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    size_t length = 0;
    const uint8_t *bytes = framedsl_encode(sender, "TM_DATAGRAM_02", &length, NULL);

    if (bytes == NULL) {
      failure = __LINE__;
    } else {
      send_it(bytes, length);

      uint8_t received[32] = { 0 };
      size_t received_length = 0;
      uint64_t value = 0;
      const char *packet = framedsl_decode(receiver, sent, sent_length, NULL, NULL);

      /* 1 byte of header, 4 of length, 12 of payload, 2 of CRC. */
      if (packet == NULL || strcmp(packet, "TM_DATAGRAM_02") != 0) {
        failure = __LINE__;
      } else if (length != 19) {
        failure = __LINE__;
      } else if (sent[0] != 0xAA) {
        failure = __LINE__; /* the discriminator, written from the definition */
      } else if (framedsl_get_bytes_into(receiver, data, received, sizeof(received), &received_length) != FRAMEDSL_OK
                 || received_length != sizeof(payload) || memcmp(received, payload, sizeof(payload)) != 0) {
        failure = __LINE__;
      } else if (framedsl_get_u64(receiver, crc, &value) != FRAMEDSL_OK || value != 0xBEEF) {
        failure = __LINE__;
      }
    }
  }

  framedsl_frame_free(sender);
  framedsl_frame_free(receiver);
  framedsl_result_free(result);
  return failure;
}

/* ---- What the frame is holding -----------------------------------------------------------------
 *
 * The arena has a slot for every field of every packet, which is what makes it one allocation. The
 * cost is that after decoding a TC_PING, TC_SET_MODE's slots still hold whatever was last in them --
 * so a caller reading the wrong packet's field gets a number rather than an answer. These three cases
 * are the difference between "do not read that" written in the manual and it being true. */

static int us6_tc12_a_frame_keeps_each_packets_last_value(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  /* A TC_SET_MODE first, then a TC_PING. Nothing is shared between two packets' slots, so the
   * second decode has no way to disturb the first, and mode still reads the value that arrived. */
  const uint8_t set_mode[] = { 0x02, 0x12, 0x34, 0x7F };
  const uint8_t ping[] = { 0x01, 0xAB, 0xCD };

  FramedslFieldId mode;
  uint64_t value = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TC_SET_MODE.mode", &mode)) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, set_mode, sizeof(set_mode), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0x7F) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, ping, sizeof(ping), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "TC_PING.seq", 0xABCD)) {
    failure = __LINE__; /* the packet that just arrived */
  } else if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0x7F) {
    failure = __LINE__; /* and the one before it, still the last thing TC_SET_MODE carried */
  }

  close_fixture(&fixture);
  return failure;
}

/* The guarantee that lets every read above be a plain read: a decode that gives up part-way puts
 * back what it disturbed, so no field is ever half of one frame and half of the next. */
static int us6_tc13_a_failed_decode_leaves_the_previous_values(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t good[] = { 0x02, 0x12, 0x34, 0x7F };
  /* Matches TC_SET_MODE and carries a whole, different seq -- so the walk writes 0x9988 over 0x1234
   * before running out on mode. Without the snapshot this is exactly where the old value is lost. */
  const uint8_t cut_after_seq[] = { 0x02, 0x99, 0x88 };
  const uint8_t noise[] = { 0xFF, 0x00, 0x00, 0x00 };

  FramedslFieldId seq;
  FramedslFieldId mode;
  uint64_t value = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TC_SET_MODE.seq", &seq)
      || !framedsl_find(fixture.definition, "TC_SET_MODE.mode", &mode)) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, good, sizeof(good), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, cut_after_seq, sizeof(cut_after_seq), NULL, NULL) != NULL) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, seq, &value) != FRAMEDSL_OK || value != 0x1234) {
    failure = __LINE__; /* not 0x9988: the half-written frame was put back */
  } else if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0x7F) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, noise, sizeof(noise), NULL, NULL) != NULL) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, seq, &value) != FRAMEDSL_OK || value != 0x1234) {
    failure = __LINE__; /* a frame that matched nothing never reached a slot in the first place */
  }

  close_fixture(&fixture);
  return failure;
}

/* A variable-width field's length lives beside its bytes, so the restore has to put both back. A
 * field left with the right bytes and a longer frame's length reads its own tail as payload. */
static int us6_tc14_a_failed_decode_restores_a_variable_length(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  /* TC_PAYLOAD: len, then that many bytes of body, then a crc. */
  const uint8_t three[] = { 0x03, 0x03, 0xAA, 0xBB, 0xCC, 0x5A };
  /* Claims five bytes of body, carries all five, and then runs out on the crc -- so the walk does
   * reach body and does write it, which is the only way this tests the restore rather than a
   * truncation that stopped short of it. */
  const uint8_t cut_after_body[] = { 0x03, 0x05, 0xDD, 0xEE, 0xFF, 0x11, 0x22 };

  FramedslFieldId body;
  uint8_t bytes[16] = { 0 };
  size_t length = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TC_PAYLOAD.body", &body)) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, three, sizeof(three), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_byte_length(fixture.frame, body) != 3) {
    failure = __LINE__;
  } else if (framedsl_decode(fixture.frame, cut_after_body, sizeof(cut_after_body), NULL, NULL) != NULL) {
    failure = __LINE__;
  } else if (framedsl_byte_length(fixture.frame, body) != 3) {
    failure = __LINE__; /* the length came back with the bytes, not from the frame that failed */
  } else if (framedsl_get_bytes_into(fixture.frame, body, bytes, sizeof(bytes), &length) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (length != 3 || bytes[0] != 0xAA || bytes[1] != 0xBB || bytes[2] != 0xCC) {
    failure = __LINE__;
  }

  close_fixture(&fixture);
  return failure;
}

/* There is no unset. A field nobody wrote and no frame carried reads zero -- the same answer for a
 * caller's frame and a decoded one, which is what removed the rule this story used to check. */
static int us6_tc15_an_untouched_field_reads_zero(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!open_fixture(FAMILY, &fixture)) {
    close_fixture(&fixture);
    FORCE_FAILURE();
  }

  const uint8_t ping[] = { 0x01, 0xAB, 0xCD };
  FramedslFieldId mode;
  uint64_t value = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TC_SET_MODE.mode", &mode)) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "TC_PING.seq", 0) || !scalar_is(&fixture, "TC_PAYLOAD.len", 0)) {
    failure = __LINE__; /* a new frame: every field of every packet reads, as zero */
  } else if (framedsl_decode(fixture.frame, ping, sizeof(ping), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0) {
    failure = __LINE__; /* a packet this frame has never carried: zero, not a refusal */
  } else if (framedsl_set_u64(fixture.frame, mode, 0x42) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0x42) {
    failure = __LINE__;
  } else if (!scalar_is(&fixture, "TC_PING.seq", 0xABCD)) {
    failure = __LINE__; /* writing one packet's field leaves another packet's decode alone */
  } else {
    framedsl_frame_reset(fixture.frame);
    if (framedsl_get_u64(fixture.frame, mode, &value) != FRAMEDSL_OK || value != 0
        || !scalar_is(&fixture, "TC_PING.seq", 0)) {
      failure = __LINE__;
    }
  }

  close_fixture(&fixture);
  return failure;
}

void register_user_story6(void) {
  START_REGISTERING();
  ADD_TEST_CASE(
    us6_tc01_hand_written_bytes_decode_to_their_values, "hand-written bytes decode to the values they hold", false
  );
  ADD_TEST_CASE(us6_tc02_the_right_packet_is_identified, "each frame is identified as its own packet", false);
  ADD_TEST_CASE(
    us6_tc03_the_failures_are_told_apart, "unrecognised, truncated and over-bound are three answers", false
  );
  ADD_TEST_CASE(us6_tc04_nothing_is_read_past_the_buffer, "an exactly-sized buffer is not read past", false);
  ADD_TEST_CASE(
    us6_tc05_a_variable_field_decodes_at_every_length,
    "a valof() field decodes at zero, a middling length and its reservation",
    false
  );
  ADD_TEST_CASE(us6_tc06_bit_and_byte_order_are_undone, "lsb_first and little_endian are read back correctly", false);
  ADD_TEST_CASE(
    us6_tc07_a_stream_can_be_walked_frame_by_frame, "consumed lets a stream be walked frame by frame", false
  );
  ADD_TEST_CASE(us6_tc08_decoding_does_not_allocate, "decoding allocates nothing, counted", false);
  ADD_TEST_CASE(us6_tc09_null_is_tolerated, "decode tolerates a NULL frame", false);
  ADD_TEST_CASE(
    us6_tc10_a_frame_survives_a_round_trip, "set, encode, send, decode, read back -- between two frames", false
  );
  ADD_TEST_CASE(us6_tc11_the_round_trip_works_from_a_file, "the same, from a definition file on disk", false);
  ADD_TEST_CASE(
    us6_tc12_a_frame_keeps_each_packets_last_value, "a frame keeps the last value each packet carried", false
  );
  ADD_TEST_CASE(
    us6_tc13_a_failed_decode_leaves_the_previous_values,
    "a failed decode puts back what it had already overwritten",
    false
  );
  ADD_TEST_CASE(
    us6_tc14_a_failed_decode_restores_a_variable_length,
    "the restore puts back a variable field's length as well as its bytes",
    false
  );
  ADD_TEST_CASE(us6_tc15_an_untouched_field_reads_zero, "a field nobody wrote and no frame carried reads zero", false);
}
