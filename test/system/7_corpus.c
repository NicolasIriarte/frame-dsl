/* Story 7: one table, one loop, the whole round trip.
 *
 * Stories 5 and 6 check encoding and decoding one behaviour at a time, and they are worth keeping
 * exactly as they are. What they cannot give is coverage that grows by adding a row: every case is a
 * function, so a new combination of features means a new function, and the combinations actually
 * worth testing -- a sub-byte field beside a little-endian one, a valof() field at exactly its
 * reservation in a packet that also carries a bit string -- are precisely the ones nobody writes a
 * function for.
 *
 * So: a table. Each row names a definition *file*, the packet, the values to set, and the bytes those
 * values must produce. One loop does four things per row:
 *
 *   1. load the file, set the fields, encode;
 *   2. compare the bytes against the row -- hand-written, worked out from the definition;
 *   3. decode that same byte array into a SECOND frame;
 *   4. check every field came back as the row says.
 *
 * Step 3 taking a separate frame is what makes step 4 mean anything. A frame that decoded into itself
 * would still be holding the values it started with, and the case would pass with no decoder at all.
 * Step 2 is what keeps the round trip honest: the bytes are the specification and the decoder is not,
 * because two matching bugs cancel.
 *
 * Definition files rather than inline strings, because that is how a user's definition arrives, it is
 * the thing this project claims can change without a rebuild, and a .fdsl file can be read and fixed
 * by somebody who is not reading C. They live in test/corpus/ rather than examples/: examples/ is
 * documentation that happens to be tested, and a fixture written to pin one combination of features
 * is not documentation. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "framedsl.h"
#include "testsuite.h"

/* ---- The table ------------------------------------------------------------------------------- */

typedef struct {
  /* Dotted, as framedsl_find() takes it: "VARIABLE.len". */
  const char *dotted;
  bool is_bytes;

  /* What the caller sets. */
  uint64_t value;
  const uint8_t *bytes;
  size_t length;

  /* What it must read back as after the round trip. Almost always the same thing -- a fixed-value
   * field is the exception, since the encoder takes it from the definition and a caller's write to
   * it never reaches the wire. */
  uint64_t reads_back;
} Field;

typedef struct {
  /* What this row exists to pin. Printed when it fails, because "row 7" is not a diagnosis. */
  const char *what;
  const char *file;
  const char *packet;

  const Field *fields;
  size_t field_count;

  const uint8_t *expected;
  size_t expected_length;
} Row;

/* A scalar, set and read back as the same value. */
#define SCALAR(dotted, value) {(dotted), false, (value), NULL, 0, (value)}
/* A scalar set to one thing and read back as another: a discriminator the caller wrote over. */
#define SCALAR_READS(dotted, set, back) {(dotted), false, (set), NULL, 0, (back)}
/* A bit string. `reads_back` is unused; the bytes are compared directly. */
#define BYTES(dotted, array) {(dotted), true, 0, (array), sizeof(array), 0}

static const uint8_t nine_bytes[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
static const uint8_t nine_nibbles[9] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11};
static const uint8_t three_bytes[3] = {1, 2, 3};
static const uint8_t eight_bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
static const uint8_t two_bytes[2] = {0xAA, 0xBB};

static const Field byte_aligned[] = {
    SCALAR("BYTE_ALIGNED.seq", 0x1234),
    SCALAR("BYTE_ALIGNED.tail", 0x5F),
};

static const Field sub_byte[] = {
    SCALAR("SUB_BYTE.sec", 1),
    SCALAR("SUB_BYTE.apid", 0x555),
    SCALAR("SUB_BYTE.seq_flags", 3),
    SCALAR("SUB_BYTE.seq_count", 1),
};

static const Field bit_order[] = {
    SCALAR("BIT_ORDER.a", 0x0F),
    SCALAR("BIT_ORDER.b", 0x0F),
};

static const Field byte_order[] = {
    SCALAR("BYTE_ORDER.big", 0x11223344),
    SCALAR("BYTE_ORDER.little", 0x11223344),
};

static const Field fixed_over[] = {
    /* Written over with 0x55, and it must come back as the 0xAA the definition pins. */
    SCALAR_READS("FIXED_OVER.hdr", 0x55, 0xAA),
    SCALAR("FIXED_OVER.body", 0x01),
};

static const Field bit_string[] = {
    BYTES("BIT_STRING.wide", nine_bytes),
};

static const Field bit_string_offset[] = {
    BYTES("BIT_STRING_OFFSET.wide", nine_nibbles),
    SCALAR("BIT_STRING_OFFSET.pad", 0x5),
};

static const Field variable_empty[] = {
    SCALAR("VARIABLE.len", 0),
    SCALAR("VARIABLE.crc", 0xC7),
};

static const Field variable_middling[] = {
    SCALAR("VARIABLE.len", 3),
    BYTES("VARIABLE.data", three_bytes),
    SCALAR("VARIABLE.crc", 0xC7),
};

static const Field variable_full[] = {
    SCALAR("VARIABLE.len", 8),
    BYTES("VARIABLE.data", eight_bytes),
    SCALAR("VARIABLE.crc", 0xC7),
};

static const Field family_ping[] = {
    SCALAR("TC_PING.seq", 0x0009),
};

static const Field family_set_mode[] = {
    SCALAR("TC_SET_MODE.seq", 0x1234),
    SCALAR("TC_SET_MODE.mode", 0x7F),
};

static const Field family_payload[] = {
    SCALAR("TC_PAYLOAD.len", 2),
    BYTES("TC_PAYLOAD.body", two_bytes),
    SCALAR("TC_PAYLOAD.crc", 0xC7),
};

static const uint8_t bytes_byte_aligned[] = {0xAA, 0x12, 0x34, 0x5F};
static const uint8_t bytes_sub_byte[] = {0x0D, 0x55, 0xC0, 0x01};
static const uint8_t bytes_bit_order[] = {0xAA, 0x0F, 0xF0};
static const uint8_t bytes_byte_order[] = {0xBB, 0x11, 0x22, 0x33, 0x44,
                                           0x44, 0x33, 0x22, 0x11, 0xBB};
static const uint8_t bytes_fixed_over[] = {0xAA, 0x01};
static const uint8_t bytes_bit_string[] = {0xAA, 1, 2, 3, 4, 5, 6, 7, 8, 9};
static const uint8_t bytes_bit_string_offset[] = {0xA1, 0x23, 0x45, 0x67, 0x89,
                                                  0xAB, 0xCD, 0xEF, 0x01, 0x15};
static const uint8_t bytes_variable_empty[] = {0xAA, 0x00, 0xC7};
static const uint8_t bytes_variable_middling[] = {0xAA, 0x03, 1, 2, 3, 0xC7};
static const uint8_t bytes_variable_full[] = {0xAA, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0xC7};
static const uint8_t bytes_family_ping[] = {0x01, 0x00, 0x09};
static const uint8_t bytes_family_set_mode[] = {0x02, 0x12, 0x34, 0x7F};
static const uint8_t bytes_family_payload[] = {0x03, 0x02, 0xAA, 0xBB, 0xC7};

#define ROW(what, file, packet, fields, bytes)                                                     \
  {(what), (file), (packet), (fields), sizeof(fields) / sizeof(*(fields)), (bytes), sizeof(bytes)}

static const Row ROWS[] = {
    ROW("whole-byte fields in declaration order", "byte_aligned.fdsl", "BYTE_ALIGNED", byte_aligned,
        bytes_byte_aligned),
    ROW("sub-byte fields packing and straddling", "sub_byte.fdsl", "SUB_BYTE", sub_byte,
        bytes_sub_byte),
    ROW("lsb_first beside msb_first", "bit_order.fdsl", "BIT_ORDER", bit_order, bytes_bit_order),
    ROW("little_endian swapping only its own field", "byte_order.fdsl", "BYTE_ORDER", byte_order,
        bytes_byte_order),
    ROW("a discriminator the caller wrote over", "fixed_over.fdsl", "FIXED_OVER", fixed_over,
        bytes_fixed_over),
    ROW("a bit string wider than a scalar", "bit_string.fdsl", "BIT_STRING", bit_string,
        bytes_bit_string),
    ROW("a bit string off a byte boundary", "bit_string_offset.fdsl", "BIT_STRING_OFFSET",
        bit_string_offset, bytes_bit_string_offset),
    ROW("a valof() field at zero", "variable.fdsl", "VARIABLE", variable_empty,
        bytes_variable_empty),
    ROW("a valof() field at a middling length", "variable.fdsl", "VARIABLE", variable_middling,
        bytes_variable_middling),
    ROW("a valof() field at exactly its reservation", "variable.fdsl", "VARIABLE", variable_full,
        bytes_variable_full),
    ROW("one of three packets, the shortest", "family.fdsl", "TC_PING", family_ping,
        bytes_family_ping),
    ROW("one of three packets, a fixed-width one", "family.fdsl", "TC_SET_MODE", family_set_mode,
        bytes_family_set_mode),
    ROW("one of three packets, the variable one", "family.fdsl", "TC_PAYLOAD", family_payload,
        bytes_family_payload),
};

#define ROW_COUNT (sizeof(ROWS) / sizeof(*ROWS))

/* ---- The loop -------------------------------------------------------------------------------- */

/* A table-driven test that reports only "failed" costs more than it saves: the reader is left
 * bisecting a table by hand. Every failure below says which row, what the row was for, and -- where
 * bytes differ -- which byte and both values. */
static void complain(const Row *row, size_t index, const char *fmt, ...) {
  va_list arguments;
  fprintf(stderr, "\n  corpus row %zu (%s, %s): %s\n    ", index, row->file, row->packet,
          row->what);
  va_start(arguments, fmt);
  vfprintf(stderr, fmt, arguments);
  va_end(arguments);
  fprintf(stderr, "\n");
}

/* The first byte at which two buffers differ, or `length` if they do not. */
static size_t first_difference(const uint8_t *left, const uint8_t *right, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    if (left[i] != right[i]) {
      return i;
    }
  }
  return length;
}

static bool set_field(FramedslFrame *frame, const FramedslDefinition *definition, const Field *field,
                      FramedslFieldId *id) {
  if (!framedsl_find(definition, field->dotted, id)) {
    return false;
  }
  if (field->is_bytes) {
    return framedsl_set_bytes(frame, *id, field->bytes, field->length) == FRAMEDSL_OK;
  }
  return framedsl_set_u64(frame, *id, field->value) == FRAMEDSL_OK;
}

/* One row, all four steps. Returns 0, or the line it gave up at, having said why. */
static int run_row(const Row *row, size_t index) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", CORPUS_DIR, row->file);

  FramedslResult *result = framedsl_load_file(path);
  const FramedslDefinition *definition = framedsl_definition(result);
  /* Two frames over one definition. The receiver never has a value put into it by hand, so anything
   * it reads back, the decoder put there. */
  FramedslFrame *sender = framedsl_frame_new(definition);
  FramedslFrame *receiver = framedsl_frame_new(definition);
  int failure = 0;

  if (!framedsl_ok(result)) {
    complain(row, index, "the definition did not load: %s",
             framedsl_diagnostic_count(result) > 0 ? framedsl_diagnostic(result, 0)->message
                                                   : "no diagnostic");
    failure = __LINE__;
  } else if (sender == NULL || receiver == NULL) {
    complain(row, index, "could not create a frame");
    failure = __LINE__;
  }

  /* 1. Set every field the row names. */
  for (size_t f = 0; failure == 0 && f < row->field_count; ++f) {
    FramedslFieldId id;
    if (!set_field(sender, definition, &row->fields[f], &id)) {
      complain(row, index, "could not set %s", row->fields[f].dotted);
      failure = __LINE__;
    }
  }

  /* 2. Encode, and compare against the row's bytes. */
  size_t length = 0;
  const uint8_t *encoded = NULL;
  if (failure == 0) {
    FramedslStatus status = FRAMEDSL_OK;
    encoded = framedsl_encode(sender, row->packet, &length, &status);

    if (encoded == NULL) {
      complain(row, index, "encode refused the frame (status %d)", (int)status);
      failure = __LINE__;
    } else if (length != row->expected_length) {
      complain(row, index, "encoded %zu bytes, expected %zu", length, row->expected_length);
      failure = __LINE__;
    } else {
      size_t at = first_difference(encoded, row->expected, length);
      if (at != length) {
        complain(row, index, "byte %zu is 0x%02X, expected 0x%02X", at, encoded[at],
                 row->expected[at]);
        failure = __LINE__;
      }
    }
  }

  /* 3. Decode the row's own bytes -- not the encoder's output -- into the other frame. If the
   * encoder is wrong, step 2 has already said so; decoding its output as well would let a matching
   * pair of bugs agree with each other. */
  if (failure == 0) {
    FramedslStatus status = FRAMEDSL_OK;
    size_t consumed = 0;
    const char *packet =
        framedsl_decode(receiver, row->expected, row->expected_length, &consumed, &status);

    if (packet == NULL) {
      complain(row, index, "decode refused the bytes (status %d)", (int)status);
      failure = __LINE__;
    } else if (strcmp(packet, row->packet) != 0) {
      complain(row, index, "decoded as %s", packet);
      failure = __LINE__;
    } else if (consumed != row->expected_length) {
      complain(row, index, "consumed %zu bytes of %zu", consumed, row->expected_length);
      failure = __LINE__;
    }
  }

  /* 4. Read every field back. */
  for (size_t f = 0; failure == 0 && f < row->field_count; ++f) {
    const Field *field = &row->fields[f];
    FramedslFieldId id;

    if (!framedsl_find(definition, field->dotted, &id)) {
      complain(row, index, "could not find %s on the way back", field->dotted);
      failure = __LINE__;
      break;
    }

    if (field->is_bytes) {
      uint8_t read[64] = {0};
      size_t read_length = 0;

      if (framedsl_get_bytes(receiver, id, read, sizeof(read), &read_length) != FRAMEDSL_OK) {
        complain(row, index, "%s could not be read back", field->dotted);
        failure = __LINE__;
      } else if (read_length != field->length) {
        complain(row, index, "%s came back %zu bytes, set as %zu", field->dotted, read_length,
                 field->length);
        failure = __LINE__;
      } else {
        size_t at = first_difference(read, field->bytes, read_length);
        if (at != read_length) {
          complain(row, index, "%s byte %zu came back 0x%02X, set as 0x%02X", field->dotted, at,
                   read[at], field->bytes[at]);
          failure = __LINE__;
        }
      }
      continue;
    }

    uint64_t value = 0;
    if (framedsl_get_u64(receiver, id, &value) != FRAMEDSL_OK) {
      complain(row, index, "%s could not be read back", field->dotted);
      failure = __LINE__;
    } else if (value != field->reads_back) {
      complain(row, index, "%s came back %llu, expected %llu", field->dotted,
               (unsigned long long)value, (unsigned long long)field->reads_back);
      failure = __LINE__;
    }
  }

  framedsl_frame_free(sender);
  framedsl_frame_free(receiver);
  framedsl_result_free(result);
  return failure;
}

static int us7_tc01_every_row_round_trips(const TestArguments *args) {
  (void)args;

  for (size_t i = 0; i < ROW_COUNT; ++i) {
    int failure = run_row(&ROWS[i], i);
    if (failure != 0) {
      return failure;
    }
  }
  return 0;
}

/* A fixture nobody loads is a fixture that has quietly stopped being valid -- it can fail to parse,
 * or describe something the language no longer has, and nothing says so. Checked here rather than by
 * eye, because "by eye" means "the first time somebody looks, which is never". */
static int us7_tc02_every_corpus_file_is_used(const TestArguments *args) {
  (void)args;

  DIR *directory = opendir(CORPUS_DIR);
  if (directory == NULL) {
    FORCE_FAILURE();
  }

  size_t seen = 0;
  int failure = 0;
  const struct dirent *entry;

  while ((entry = readdir(directory)) != NULL) {
    const char *dot = strrchr(entry->d_name, '.');
    if (dot == NULL || strcmp(dot, ".fdsl") != 0) {
      continue;
    }
    seen += 1;

    bool used = false;
    for (size_t i = 0; i < ROW_COUNT && !used; ++i) {
      used = strcmp(ROWS[i].file, entry->d_name) == 0;
    }
    if (!used) {
      fprintf(stderr, "\n  %s/%s is named by no row\n", CORPUS_DIR, entry->d_name);
      failure = __LINE__;
    }
  }
  closedir(directory);

  /* And the directory was actually read: a scan that found nothing would pass the loop above without
   * checking anything at all. */
  if (failure == 0 && seen < 8) {
    fprintf(stderr, "\n  only %zu corpus files found in %s\n", seen, CORPUS_DIR);
    failure = __LINE__;
  }
  return failure;
}

/* The table is only evidence if the comparison it makes can fail. Feeding a row's own bytes back
 * with one bit changed must be noticed -- by the byte comparison, or by the packet no longer being
 * recognised, both of which are the table doing its job. */
static int us7_tc03_a_wrong_byte_would_be_caught(const TestArguments *args) {
  (void)args;

  for (size_t i = 0; i < ROW_COUNT; ++i) {
    const Row *row = &ROWS[i];
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", CORPUS_DIR, row->file);

    FramedslResult *result = framedsl_load_file(path);
    FramedslFrame *frame = framedsl_frame_new(framedsl_definition(result));
    int failure = 0;

    if (frame == NULL) {
      failure = __LINE__;
    } else {
      /* The last byte, which is a value in every row rather than a discriminator. */
      uint8_t corrupted[64] = {0};
      memcpy(corrupted, row->expected, row->expected_length);
      corrupted[row->expected_length - 1] ^= 0xFF;

      FramedslStatus status = FRAMEDSL_OK;
      const char *packet =
          framedsl_decode(frame, corrupted, row->expected_length, NULL, &status);

      /* Either it is no longer this packet, or it is and something differs. What must not happen is
       * the altered bytes producing the row's values anyway, which would mean the comparison is
       * reading something other than the frame. */
      if (packet != NULL && strcmp(packet, row->packet) == 0) {
        if (first_difference(corrupted, row->expected, row->expected_length) ==
            row->expected_length) {
          failure = __LINE__;
        }
      }
    }

    framedsl_frame_free(frame);
    framedsl_result_free(result);
    if (failure != 0) {
      return failure;
    }
  }
  return 0;
}

void register_user_story7(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us7_tc01_every_row_round_trips,
                "every row encodes to its bytes and decodes back to its values", true);
  ADD_TEST_CASE(us7_tc02_every_corpus_file_is_used, "every file in the corpus is named by a row",
                false);
  ADD_TEST_CASE(us7_tc03_a_wrong_byte_would_be_caught, "a changed byte does not go unnoticed",
                false);
}
