/* Story 4: the arena and the accessors, used the way a program uses them.
 *
 * A system test, so the only header from this project is the public one. That is the point here more
 * than anywhere else: if reading a value back needed something internal, the accessors would not be
 * finished.
 *
 * The allocator is interposed in main.c, so "nothing allocates after the frame exists" is measured
 * rather than asserted. See allocations.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocations.h"
#include "framedsl.h"
#include "testsuite.h"

#define DEFINITION                                                                                                     \
  "@bit_order:  msb_first;\n"                                                                                          \
  "@byte_order: big_endian;\n"                                                                                         \
  "pkt TM_A {\n"                                                                                                       \
  "  hdr:       8 = 0xAA;\n"                                                                                           \
  "  len:      32;\n"                                                                                                  \
  "  wide:     72;\n" /* over 64 bits: a bit string, nine bytes */                                                     \
  "  data[16]: valof(len) * 8;\n"                                                                                      \
  "  crc:      16;\n"                                                                                                  \
  "};\n"                                                                                                               \
  "pkt TM_B { hdr: 8 = 0xBB; seq: 16; };\n"

/* ---- Helpers --------------------------------------------------------------------------------- */

typedef struct {
  FramedslResult *result;
  const FramedslDefinition *definition;
  FramedslFrame *frame;
} Fixture;

static bool fixture_open(Fixture *fixture) {
  fixture->result = framedsl_load_string(DEFINITION, "frame.fdsl");
  fixture->definition = framedsl_definition(fixture->result);
  fixture->frame = framedsl_frame_new(fixture->definition);
  return framedsl_ok(fixture->result) && fixture->frame != NULL;
}

static void fixture_close(Fixture *fixture) {
  framedsl_frame_free(fixture->frame);
  framedsl_result_free(fixture->result);
}

/* ---- Cases ----------------------------------------------------------------------------------- */

/* Zeroed, and zeroed everywhere: a frame covers every field of every packet, not just the first. */
static int us4_tc01_a_new_frame_reads_zero_everywhere(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  int failure = 0;
  for (size_t p = 0; p < framedsl_packet_count(fixture.definition) && failure == 0; ++p) {
    for (size_t f = 0; f < framedsl_field_count(fixture.definition, p) && failure == 0; ++f) {
      FramedslFieldId id;
      FramedslField described;
      if (!framedsl_field_id(fixture.definition, p, f, &id) || !framedsl_field(fixture.definition, p, f, &described)) {
        failure = __LINE__;
        break;
      }
      if (!described.is_scalar) {
        continue;
      }
      uint64_t value = 1;
      if (framedsl_get_u64(fixture.frame, id, &value) != FRAMEDSL_OK || value != 0) {
        failure = __LINE__;
      }
    }
  }

  fixture_close(&fixture);
  return failure;
}

static int us4_tc02_a_scalar_round_trips(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  FramedslFieldId len;
  int failure = 0;
  uint64_t value = 0;

  if (!framedsl_find(fixture.definition, "TM_A.len", &len)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, len, 0xDEADBEEF) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_u64(fixture.frame, len, &value) != FRAMEDSL_OK || value != 0xDEADBEEF) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

/* Rejected before anything is written, so the slot keeps what it had rather than half a value. */
static int us4_tc03_a_value_too_wide_is_refused_without_writing(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  FramedslFieldId hdr;
  int failure = 0;
  uint64_t value = 0;

  if (!framedsl_find(fixture.definition, "TM_A.hdr", &hdr)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, hdr, 0x5A) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, hdr, 0x1FF) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__; /* 0x1FF needs nine bits and the field is eight */
  } else if (framedsl_get_u64(fixture.frame, hdr, &value) != FRAMEDSL_OK || value != 0x5A) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

/* The width alone decides which pair of accessors applies, so using the wrong one is an error rather
 * than a reinterpretation of the bytes. */
static int us4_tc04_the_wrong_accessor_for_the_field_is_an_error(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  FramedslFieldId len;
  FramedslFieldId wide;
  uint8_t buffer[16] = { 0 };
  size_t length = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.len", &len) || !framedsl_find(fixture.definition, "TM_A.wide", &wide)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, wide, 1) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* 72 bits is a bit string */
  } else if (framedsl_set_bytes(fixture.frame, len, buffer, 4) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* 32 bits is a scalar */
  } else if (framedsl_get_bytes_into(fixture.frame, len, buffer, sizeof(buffer), &length) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

static int us4_tc05_a_wide_fixed_field_round_trips(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  /* 72 bits is nine bytes, and a fixed-width field takes exactly its own size. */
  const uint8_t written[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  uint8_t read[16] = { 0 };
  size_t length = 0;
  FramedslFieldId wide;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.wide", &wide)) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, wide, written, sizeof(written)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_bytes_into(fixture.frame, wide, read, sizeof(read), &length) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (length != sizeof(written) || memcmp(read, written, sizeof(written)) != 0) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, wide, written, 4) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__; /* short of its own size: the rest would keep whatever was there */
  }

  fixture_close(&fixture);
  return failure;
}

/* A variable field's length is whatever last arrived, including none of it. */
static int us4_tc06_a_variable_field_remembers_its_length(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[5] = { 9, 8, 7, 6, 5 };
  uint8_t read[32] = { 0 };
  size_t length = 99;
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data)) {
    failure = __LINE__;
  } else if (framedsl_byte_length(fixture.frame, data) != 0) {
    failure = __LINE__; /* starts empty, and empty is a length it can have */
  } else if (framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_byte_length(fixture.frame, data) != sizeof(payload)) {
    failure = __LINE__;
  } else if (framedsl_get_bytes_into(fixture.frame, data, read, sizeof(read), &length) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (length != sizeof(payload) || memcmp(read, payload, sizeof(payload)) != 0) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, payload, 0) != FRAMEDSL_OK
             || framedsl_byte_length(fixture.frame, data) != 0) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

/* A shorter value must not leave the tail of a longer one visible. */
static int us4_tc07_a_shorter_value_clears_what_it_replaced(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t longer[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
  const uint8_t shorter[2] = { 2, 2 };
  uint8_t read[32] = { 0 };
  size_t length = 0;
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data)) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, longer, sizeof(longer)) != FRAMEDSL_OK
             || framedsl_set_bytes(fixture.frame, data, shorter, sizeof(shorter)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_bytes_into(fixture.frame, data, read, sizeof(read), &length) != FRAMEDSL_OK
             || length != sizeof(shorter)) {
    failure = __LINE__;
  } else {
    /* Read the whole reservation back the long way to see what is behind the length. */
    if (framedsl_set_bytes(fixture.frame, data, read, 16) != FRAMEDSL_OK) {
      failure = __LINE__;
    } else if (framedsl_get_bytes_into(fixture.frame, data, read, sizeof(read), &length) != FRAMEDSL_OK) {
      failure = __LINE__;
    } else if (read[2] != 0 || read[7] != 0) {
      failure = __LINE__; /* the old value's tail survived */
    }
  }

  fixture_close(&fixture);
  return failure;
}

static int us4_tc08_over_the_reservation_is_refused(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  uint8_t payload[32] = { 0 };
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data)) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, payload, 16) != FRAMEDSL_OK) {
    failure = __LINE__; /* exactly the reservation is fine */
  } else if (framedsl_set_bytes(fixture.frame, data, payload, 17) != FRAMEDSL_ERROR_RANGE) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

/* A caller who guessed the buffer size wrong is told the size, not just refused. */
static int us4_tc09_a_small_buffer_reports_the_size_needed(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
  uint8_t small[3] = { 0 };
  size_t length = 0;
  FramedslFieldId data;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data)) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_bytes_into(fixture.frame, data, small, sizeof(small), &length) != FRAMEDSL_ERROR_BUFFER) {
    failure = __LINE__;
  } else if (length != sizeof(payload)) {
    failure = __LINE__;
  }

  fixture_close(&fixture);
  return failure;
}

static int us4_tc10_find_resolves_and_misses_cleanly(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  FramedslFieldId id;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_B.seq", &id)) {
    failure = __LINE__;
  } else if (framedsl_find(fixture.definition, "NOPE.seq", &id)) {
    failure = __LINE__;
  } else if (framedsl_find(fixture.definition, "TM_B.nope", &id)) {
    failure = __LINE__;
  } else if (framedsl_find(fixture.definition, "seq", &id)) {
    failure = __LINE__; /* no dot at all */
  } else if (framedsl_find(fixture.definition, "TM_B.", &id) || framedsl_find(fixture.definition, ".seq", &id)) {
    failure = __LINE__;
  } else if (framedsl_find(fixture.definition, "TM_B.a.seq", &id)) {
    failure = __LINE__; /* nothing nests yet, so it simply finds nothing */
  }

  fixture_close(&fixture);
  return failure;
}

/* Two frames over one definition are independent, and an id from another definition is refused
 * rather than addressing whatever sits at that index. */
static int us4_tc11_frames_and_definitions_do_not_leak_into_each_other(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  FramedslResult *other_result = framedsl_load_string(DEFINITION, "other.fdsl");
  const FramedslDefinition *other = framedsl_definition(other_result);
  FramedslFrame *second = framedsl_frame_new(fixture.definition);

  FramedslFieldId len;
  FramedslFieldId other_len;
  uint64_t value = 0;
  int failure = 0;

  if (second == NULL || other == NULL) {
    failure = __LINE__;
  } else if (!framedsl_find(fixture.definition, "TM_A.len", &len) || !framedsl_find(other, "TM_A.len", &other_len)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, len, 7) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_u64(second, len, &value) != FRAMEDSL_OK || value != 0) {
    failure = __LINE__; /* the second frame has its own slots */
  } else if (framedsl_set_u64(fixture.frame, other_len, 1) != FRAMEDSL_ERROR_FIELD) {
    failure = __LINE__; /* same name, same index, different definition */
  } else if (framedsl_get_u64(fixture.frame, len, &value) != FRAMEDSL_OK || value != 7) {
    failure = __LINE__;
  }

  framedsl_frame_free(second);
  framedsl_result_free(other_result);
  fixture_close(&fixture);
  return failure;
}

static int us4_tc12_reset_returns_the_frame_to_new(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[4] = { 1, 2, 3, 4 };
  FramedslFieldId len;
  FramedslFieldId data;
  uint64_t value = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.len", &len) || !framedsl_find(fixture.definition, "TM_A.data", &data)) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, len, 4) != FRAMEDSL_OK
             || framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    framedsl_frame_reset(fixture.frame);
    if (framedsl_get_u64(fixture.frame, len, &value) != FRAMEDSL_OK || value != 0) {
      failure = __LINE__;
    } else if (framedsl_byte_length(fixture.frame, data) != 0) {
      failure = __LINE__;
    }
  }

  fixture_close(&fixture);
  return failure;
}

/* The claim the whole arena design exists for. Measured by counting calls to the allocator, not by
 * reading the code and believing it. */
static int us4_tc13_nothing_allocates_after_the_frame_exists(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[6] = { 1, 2, 3, 4, 5, 6 };
  uint8_t read[32] = { 0 };
  size_t length = 0;
  FramedslFieldId len;
  FramedslFieldId data;
  FramedslFieldId wide;
  uint64_t value = 0;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.len", &len) || !framedsl_find(fixture.definition, "TM_A.data", &data)
      || !framedsl_find(fixture.definition, "TM_A.wide", &wide)) {
    failure = __LINE__;
  } else {
    /* Prove the counter is live before trusting a zero from it. An interposition that silently did
     * not take would make the rest of this case pass while measuring nothing, which is a worse
     * outcome than no test at all -- so first check that something known to allocate is seen. */
    framedsl_allocation_count_start();
    FramedslFrame *probe = framedsl_frame_new(fixture.definition);
    size_t seen = framedsl_allocation_count_stop();
    framedsl_frame_free(probe);

    if (probe == NULL || seen == 0) {
      fixture_close(&fixture);
      FORCE_FAILURE();
    }

    framedsl_allocation_count_start();

    for (int i = 0; i < 64; ++i) {
      framedsl_set_u64(fixture.frame, len, (uint64_t)i);
      framedsl_get_u64(fixture.frame, len, &value);
      framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload));
      framedsl_get_bytes_into(fixture.frame, data, read, sizeof(read), &length);
      framedsl_byte_length(fixture.frame, data);
      framedsl_frame_reset(fixture.frame);
    }

    if (framedsl_allocation_count_stop() != 0) {
      failure = __LINE__;
    }
  }

  fixture_close(&fixture);
  return failure;
}

/* A caller whose load failed is holding NULL, and finding out should not mean crashing. */
static int us4_tc14_null_is_tolerated_everywhere(const TestArguments *args) {
  (void)args;

  FramedslFieldId id = { 0 };
  uint64_t value = 0;
  uint8_t buffer[4] = { 0 };
  size_t length = 0;

  if (framedsl_frame_new(NULL) != NULL) {
    FORCE_FAILURE();
  }
  framedsl_frame_free(NULL);
  framedsl_frame_reset(NULL);

  if (framedsl_find(NULL, "A.b", &id) || framedsl_field_id(NULL, 0, 0, &id)) {
    FORCE_FAILURE();
  }
  if (framedsl_set_u64(NULL, id, 1) != FRAMEDSL_ERROR_MEMORY
      || framedsl_get_u64(NULL, id, &value) != FRAMEDSL_ERROR_MEMORY
      || framedsl_set_bytes(NULL, id, buffer, sizeof(buffer)) != FRAMEDSL_ERROR_MEMORY
      || framedsl_get_bytes_into(NULL, id, buffer, sizeof(buffer), &length) != FRAMEDSL_ERROR_MEMORY) {
    FORCE_FAILURE();
  }
  if (framedsl_byte_length(NULL, id) != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

/* The no-copy read. The bytes are already in the arena in their final form, so the caller is given
 * a pointer to them rather than a buffer to fill -- the counterpart of framedsl_encode(). */
static int us4_tc15_get_bytes_points_into_the_frame(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  FramedslFieldId data;
  FramedslFieldId len;
  size_t length = 999;
  FramedslStatus status = FRAMEDSL_OK;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data) || !framedsl_find(fixture.definition, "TM_A.len", &len)) {
    failure = __LINE__;
  } else if (framedsl_get_bytes(fixture.frame, data, &length, &status) == NULL || length != 0
             || status != FRAMEDSL_OK) {
    /* A field holding nothing is not a failure: a usable pointer and a length of zero, because NULL
     * is reserved for a call that did not work. */
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else {
    const uint8_t *first = framedsl_get_bytes(fixture.frame, data, &length, &status);
    const uint8_t *again = framedsl_get_bytes(fixture.frame, data, &length, &status);

    if (first == NULL || length != sizeof(payload) || memcmp(first, payload, sizeof(payload)) != 0) {
      failure = __LINE__;
    } else if (first != again) {
      failure = __LINE__; /* the same field is the same address, every time */
    } else if (framedsl_get_bytes(fixture.frame, len, &length, &status) != NULL || status != FRAMEDSL_ERROR_FIELD) {
      failure = __LINE__; /* a scalar is the other pair of accessors */
    } else if (framedsl_get_bytes(NULL, data, &length, &status) != NULL || status != FRAMEDSL_ERROR_MEMORY) {
      failure = __LINE__;
    } else if (framedsl_get_bytes(fixture.frame, data, NULL, &status) != NULL || status != FRAMEDSL_ERROR_MEMORY) {
      failure = __LINE__; /* the length is not optional: without it the pointer says nothing */
    } else if (framedsl_get_bytes(fixture.frame, data, &length, NULL) == NULL) {
      failure = __LINE__; /* the status is optional, as it is on framedsl_encode() */
    }
  }

  fixture_close(&fixture);
  return failure;
}

/* What the pointer promises: the address is fixed for the life of the frame, and only a write to
 * that same field changes what is behind it. Anything else would make it unusable to hold. */
static int us4_tc16_the_address_is_fixed_and_only_its_own_field_moves_it(const TestArguments *args) {
  (void)args;

  Fixture fixture;
  if (!fixture_open(&fixture)) {
    FORCE_FAILURE();
  }

  const uint8_t payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
  /* A whole TM_B, which is a different packet and so a different run of slots. */
  const uint8_t other_packet[] = { 0xBB, 0x12, 0x34 };

  FramedslFieldId data;
  FramedslFieldId len;
  size_t length = 0;
  const uint8_t *fixed = NULL;
  int failure = 0;

  if (!framedsl_find(fixture.definition, "TM_A.data", &data) || !framedsl_find(fixture.definition, "TM_A.len", &len)) {
    failure = __LINE__;
  } else if (framedsl_set_bytes(fixture.frame, data, payload, sizeof(payload)) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if ((fixed = framedsl_get_bytes(fixture.frame, data, &length, NULL)) == NULL) {
    failure = __LINE__;
  } else if (framedsl_set_u64(fixture.frame, len, 4) != FRAMEDSL_OK) {
    failure = __LINE__;
  } else if (framedsl_get_bytes(fixture.frame, data, &length, NULL) != fixed || length != sizeof(payload)
             || memcmp(fixed, payload, sizeof(payload)) != 0) {
    failure = __LINE__; /* writing another field of the same packet moves nothing */
  } else if (framedsl_decode(fixture.frame, other_packet, sizeof(other_packet), NULL, NULL) == NULL) {
    failure = __LINE__;
  } else if (framedsl_get_bytes(fixture.frame, data, &length, NULL) != fixed || length != sizeof(payload)
             || memcmp(fixed, payload, sizeof(payload)) != 0) {
    failure = __LINE__; /* nor does decoding a different packet */
  } else {
    /* A reset does change what is behind it -- and still not where it is. */
    framedsl_frame_reset(fixture.frame);
    if (framedsl_get_bytes(fixture.frame, data, &length, NULL) != fixed || length != 0) {
      failure = __LINE__;
    }
  }

  fixture_close(&fixture);
  return failure;
}

void register_user_story4(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us4_tc01_a_new_frame_reads_zero_everywhere, "a new frame is zero everywhere", false);
  ADD_TEST_CASE(us4_tc02_a_scalar_round_trips, "a scalar set comes back", false);
  ADD_TEST_CASE(
    us4_tc03_a_value_too_wide_is_refused_without_writing,
    "a value wider than the field is refused and changes nothing",
    false
  );
  ADD_TEST_CASE(
    us4_tc04_the_wrong_accessor_for_the_field_is_an_error, "the width alone decides which accessors apply", false
  );
  ADD_TEST_CASE(
    us4_tc05_a_wide_fixed_field_round_trips, "a field over 64 bits round-trips as bytes, at exactly its size", false
  );
  ADD_TEST_CASE(us4_tc06_a_variable_field_remembers_its_length, "a variable field remembers how much arrived", false);
  ADD_TEST_CASE(
    us4_tc07_a_shorter_value_clears_what_it_replaced, "a shorter value does not leave the old tail behind", false
  );
  ADD_TEST_CASE(us4_tc08_over_the_reservation_is_refused, "exactly the reservation fits, one more does not", false);
  ADD_TEST_CASE(us4_tc09_a_small_buffer_reports_the_size_needed, "a buffer too small reports the size needed", false);
  ADD_TEST_CASE(us4_tc10_find_resolves_and_misses_cleanly, "a dotted name resolves, and a miss is a miss", false);
  ADD_TEST_CASE(
    us4_tc11_frames_and_definitions_do_not_leak_into_each_other,
    "frames are independent and ids are not portable between definitions",
    false
  );
  ADD_TEST_CASE(us4_tc12_reset_returns_the_frame_to_new, "reset returns the frame to new", false);
  ADD_TEST_CASE(
    us4_tc13_nothing_allocates_after_the_frame_exists, "no allocation after the frame exists, counted", false
  );
  ADD_TEST_CASE(us4_tc14_null_is_tolerated_everywhere, "every entry point tolerates NULL", false);
  ADD_TEST_CASE(
    us4_tc15_get_bytes_points_into_the_frame,
    "reading a bit string hands back a pointer into the frame, not a copy",
    false
  );
  ADD_TEST_CASE(
    us4_tc16_the_address_is_fixed_and_only_its_own_field_moves_it,
    "that pointer is the same every time, and only its own field changes what is behind it",
    false
  );
}
