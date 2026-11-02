/* The performance baseline (#102): what encoding, decoding and field access cost, swept over generated
 * definitions, measured often enough to report a distribution rather than a number.
 *
 * `make bench` (bench/bench.c) times a dozen operations once each on one hand-written definition. That
 * spots a gross regression and cannot decide whether a change to the runtime is worth making: one run
 * is one batch, and run-to-run noise on one machine was about 10%. This program is the other half:
 *
 *   - Definitions are generated, one per point, sweeping one dimension at a time with the others held at
 *     a small default -- packet size, fields per packet, repetitions, packets per definition, and the
 *     shape of a field. See the generators below.
 *   - Every generated definition is loaded and checked to round-trip one frame before anything is timed:
 *     it encodes to the length the generator meant, decodes back to the same packet and values, and
 *     re-encodes to the same bytes. A generator that broke cannot produce a fast, meaningless number.
 *   - Each point is timed as many samples, each a batch of iterations sized to take about 2 ms so the
 *     clock's own cost disappears. The samples come from several separate processes (20 of 15 samples
 *     by default; 3 of 7 with --quick), pooled: a new process lays memory out at new random addresses,
 *     and that alone moves some points by up to 30% (see spawn()). Within a process the samples are
 *     taken in rounds -- one of every point, then the next round, in a shuffled order each time -- so a
 *     slow drift in the machine spreads over every point instead of landing on whichever ran then.
 *   - The run pins itself to one CPU, and records the machine, the governor, the compiler, the flags and
 *     the commit with the results, so two files can be compared knowing what each was.
 *
 * Results go to a CSV file (--out) that scripts/perf_compare.py compares; a summary goes to stdout.
 * Written against include/framedsl.h alone and linked against the static archive, like bench.c: what
 * is measured is what a caller gets. Not a test, and not run in CI: nothing here fails on a slow result.
 * It does fail on a wrong one. */
#define _GNU_SOURCE
#include <inttypes.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "framedsl.h"

#ifndef BASELINE_CFLAGS
  #define BASELINE_CFLAGS "unknown"
#endif
#ifndef BASELINE_CC
  #define BASELINE_CC "unknown"
#endif

/* Written to so the compiler cannot conclude a result is unused. */
static volatile uint64_t sink;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void die(const char *fmt, ...) {
  va_list arguments;
  va_start(arguments, fmt);
  fputs("baseline: ", stderr);
  vfprintf(stderr, fmt, arguments);
  fputc('\n', stderr);
  va_end(arguments);
  exit(1);
}

static void *allocate(size_t size) {
  void *memory = calloc(1, size);
  if (memory == NULL) {
    die("out of memory");
  }
  return memory;
}

/* ---- Text ----------------------------------------------------------------------------------------- */

/* A growable string, for writing definitions. */
typedef struct {
  char *text;
  size_t length;
  size_t capacity;
} Text;

static void append(Text *text, const char *fmt, ...) {
  for (;;) {
    va_list arguments;
    va_start(arguments, fmt);
    size_t room = text->capacity - text->length;
    int written = vsnprintf(text->text == NULL ? NULL : text->text + text->length, room, fmt, arguments);
    va_end(arguments);
    if (written < 0) {
      die("could not format a definition");
    }
    if ((size_t)written < room) {
      text->length += (size_t)written;
      return;
    }
    text->capacity = (text->capacity + (size_t)written + 1) * 2;
    text->text = realloc(text->text, text->capacity);
    if (text->text == NULL) {
      die("out of memory");
    }
  }
}

/* ---- A generated definition, and what is timed against it ------------------------------------------- */

/* The most elements a repetition in this sweep has, and so the most a member id table holds. */
#define MAX_ELEMENTS 1024

/* One generated definition, loaded, with everything the operations timed against it need resolved in
 * advance -- as a program would resolve its field ids once, at startup. */
typedef struct {
  char *source;
  FramedslResult *result;
  const FramedslDefinition *definition;
  /* `tx` is the caller building frames; `rx` the one receiving them. Separate, so a decode is never
   * into the frame that holds the values it is being compared against. */
  FramedslFrame *tx;
  FramedslFrame *rx;

  /* The packet encoded, and for the packets dimension the last one declared too. */
  const char *first;
  const char *last;
  /* Their encoded bytes, captured during the check, for the decode operations to read. */
  uint8_t *wire_first;
  size_t length_first;
  uint8_t *wire_last;
  size_t length_last;

  /* The scalar the accessors read and write, and the values that fit it. */
  FramedslFieldId scalar;
  uint64_t scalar_mask;

  /* A calibrated scalar, and engineering values its inverse turns into raw values that fit. */
  bool has_cal;
  FramedslFieldId cal;
  double eng[16];

  /* A repetition's member with no element chosen, and how many elements a frame carries. */
  bool has_member;
  FramedslFieldId member;
  size_t elements;
  size_t next_element;
} Subject;

/* An operation: one call, or one caller-shaped sequence of calls, on a subject. Returns nonzero when a
 * call failed, which stops the run -- a failed call is fast, and a fast failure is the meaningless
 * number the check exists to rule out. */
typedef int (*Operation)(Subject *subject, uint64_t i);

static int encode_first(Subject *s, uint64_t i) {
  (void)i;
  size_t length;
  const uint8_t *bytes = framedsl_encode(s->tx, s->first, &length, NULL);
  if (bytes == NULL) {
    return 1;
  }
  sink += bytes[length - 1];
  return 0;
}

static int encode_last(Subject *s, uint64_t i) {
  (void)i;
  size_t length;
  const uint8_t *bytes = framedsl_encode(s->tx, s->last, &length, NULL);
  if (bytes == NULL) {
    return 1;
  }
  sink += bytes[length - 1];
  return 0;
}

static int decode_first(Subject *s, uint64_t i) {
  (void)i;
  size_t consumed;
  const char *name = framedsl_decode(s->rx, s->wire_first, s->length_first, &consumed, NULL);
  sink += consumed;
  return name == NULL;
}

static int decode_last(Subject *s, uint64_t i) {
  (void)i;
  size_t consumed;
  const char *name = framedsl_decode(s->rx, s->wire_last, s->length_last, &consumed, NULL);
  sink += consumed;
  return name == NULL;
}

static int set_u64(Subject *s, uint64_t i) {
  return framedsl_set_u64(s->tx, s->scalar, i & s->scalar_mask) != FRAMEDSL_OK;
}

static int get_u64(Subject *s, uint64_t i) {
  (void)i;
  uint64_t value;
  FramedslStatus status = framedsl_get_u64(s->rx, s->scalar, &value);
  sink += value;
  return status != FRAMEDSL_OK;
}

static int set_eng(Subject *s, uint64_t i) {
  return framedsl_set_eng(s->tx, s->cal, s->eng[i & 15]) != FRAMEDSL_OK;
}

static int get_eng(Subject *s, uint64_t i) {
  (void)i;
  double value;
  FramedslStatus status = framedsl_get_eng(s->rx, s->cal, &value);
  sink += (uint64_t)value;
  return status != FRAMEDSL_OK;
}

/* One element's member, the next element each call: framedsl_element() then framedsl_get_u64(), as a
 * loop over a decoded repetition does it. */
static int element_get(Subject *s, uint64_t i) {
  (void)i;
  FramedslFieldId element;
  if (framedsl_element(s->definition, s->member, s->next_element, &element) != FRAMEDSL_OK) {
    return 1;
  }
  s->next_element = s->next_element + 1 == s->elements ? 0 : s->next_element + 1;
  uint64_t value;
  FramedslStatus status = framedsl_get_u64(s->rx, element, &value);
  sink += value;
  return status != FRAMEDSL_OK;
}

/* Every element's member: framedsl_count(), then the loop above over all of them. */
static int elements_all(Subject *s, uint64_t i) {
  (void)i;
  size_t count;
  if (framedsl_count(s->rx, s->member, &count) != FRAMEDSL_OK) {
    return 1;
  }
  for (size_t e = 0; e < count; ++e) {
    FramedslFieldId element;
    uint64_t value;
    if (framedsl_element(s->definition, s->member, e, &element) != FRAMEDSL_OK
        || framedsl_get_u64(s->rx, element, &value) != FRAMEDSL_OK) {
      return 1;
    }
    sink += value;
  }
  return 0;
}

/* ---- Points ---------------------------------------------------------------------------------------- */

/* One line of the results: an operation on a subject, and its samples. */
typedef struct {
  const char *dimension;
  char variant[32];
  uint64_t value;
  const char *operation;
  Operation run;
  Subject *subject;

  uint64_t iterations;
  double *samples; /* ns per operation, one per round */
} Point;

typedef struct {
  Point *points;
  size_t count;
  size_t capacity;
  Subject **subjects;
  size_t subject_count;
  size_t subject_capacity;
} Sweep;

static void add_point(
  Sweep *sweep,
  Subject *subject,
  const char *dimension,
  const char *variant,
  uint64_t value,
  const char *operation,
  Operation run
) {
  if (sweep->count == sweep->capacity) {
    sweep->capacity = sweep->capacity == 0 ? 64 : sweep->capacity * 2;
    sweep->points = realloc(sweep->points, sweep->capacity * sizeof(*sweep->points));
    if (sweep->points == NULL) {
      die("out of memory");
    }
  }
  Point *point = &sweep->points[sweep->count++];
  memset(point, 0, sizeof(*point));
  point->dimension = dimension;
  snprintf(point->variant, sizeof(point->variant), "%s", variant);
  point->value = value;
  point->operation = operation;
  point->run = run;
  point->subject = subject;
}

/* ---- Loading and checking a subject ----------------------------------------------------------------- */

static FramedslFieldId find(const Subject *s, const char *dotted) {
  FramedslFieldId id;
  if (!framedsl_find(s->definition, dotted, &id)) {
    die("generated definition has no field %s:\n%s", dotted, s->source);
  }
  return id;
}

static void set(Subject *s, const char *dotted, uint64_t value) {
  if (framedsl_set_u64(s->tx, find(s, dotted), value) != FRAMEDSL_OK) {
    die("could not set %s to %" PRIu64 ":\n%s", dotted, value, s->source);
  }
}

/* Deterministic bytes that are not all alike, so a bit string that lands in the wrong place differs. */
static void set_pattern(Subject *s, const char *dotted, size_t length) {
  uint8_t *bytes = allocate(length == 0 ? 1 : length);
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = (uint8_t)(i * 37 + 11);
  }
  if (framedsl_set_bytes(s->tx, find(s, dotted), bytes, length) != FRAMEDSL_OK) {
    die("could not set %zu bytes on %s:\n%s", length, dotted, s->source);
  }
  free(bytes);
}

static Subject *load(Sweep *sweep, Text *text) {
  Subject *s = allocate(sizeof(*s));
  s->source = text->text;
  text->text = NULL;
  text->length = text->capacity = 0;

  s->result = framedsl_load_string(s->source, "generated.fdsl");
  if (!framedsl_ok(s->result)) {
    for (size_t i = 0; i < framedsl_diagnostic_count(s->result); ++i) {
      const FramedslDiagnostic *d = framedsl_diagnostic(s->result, i);
      fprintf(stderr, "generated.fdsl:%d:%d: %s\n", d->line, d->column, d->message);
    }
    die("a generated definition did not load:\n%s", s->source);
  }
  s->definition = framedsl_definition(s->result);
  s->tx = framedsl_frame_new(s->definition);
  s->rx = framedsl_frame_new(s->definition);
  if (s->tx == NULL || s->rx == NULL) {
    die("out of memory");
  }
  s->first = s->last = framedsl_packet_name(s->definition, 0);

  if (sweep->subject_count == sweep->subject_capacity) {
    sweep->subject_capacity = sweep->subject_capacity == 0 ? 64 : sweep->subject_capacity * 2;
    sweep->subjects = realloc(sweep->subjects, sweep->subject_capacity * sizeof(*sweep->subjects));
    if (sweep->subjects == NULL) {
      die("out of memory");
    }
  }
  sweep->subjects[sweep->subject_count++] = s;
  return s;
}

/* Every non-fixed field of `packet` the receiver decoded, against what the sender set: scalars by
 * value, bit strings byte for byte, and each repetition's members element by element. */
static void compare_fields(Subject *s, const char *packet) {
  size_t index;
  if (!framedsl_find_packet(s->definition, packet, &index)) {
    die("no packet %s", packet);
  }
  for (size_t f = 0; f < framedsl_field_count(s->definition, index); ++f) {
    FramedslField field;
    FramedslFieldId id;
    framedsl_field(s->definition, index, f, &field);
    framedsl_field_id(s->definition, index, f, &id);
    if (field.has_fixed) {
      continue;
    }
    /* A conditional field (#12) is compared for presence first; one neither frame carries has no value. */
    if (field.condition != NULL) {
      bool sent, received;
      if (framedsl_present(s->tx, id, &sent) != FRAMEDSL_OK || framedsl_present(s->rx, id, &received) != FRAMEDSL_OK
          || sent != received) {
        die("%s.%s is present in one frame and not the other:\n%s", packet, field.name, s->source);
      }
      if (!sent) {
        continue;
      }
    }
    if (field.is_scalar) {
      uint64_t sent, received;
      if (framedsl_get_u64(s->tx, id, &sent) != FRAMEDSL_OK || framedsl_get_u64(s->rx, id, &received) != FRAMEDSL_OK
          || sent != received) {
        die(
          "%s.%s did not round-trip (%" PRIu64 " sent, %" PRIu64 " back):\n%s",
          packet,
          field.name,
          sent,
          received,
          s->source
        );
      }
    } else if (field.element_bits == 0) {
      size_t sent_length, received_length;
      const uint8_t *sent = framedsl_get_bytes(s->tx, id, &sent_length, NULL);
      const uint8_t *received = framedsl_get_bytes(s->rx, id, &received_length, NULL);
      if (sent == NULL || received == NULL || sent_length != received_length
          || memcmp(sent, received, sent_length) != 0) {
        die("%s.%s did not round-trip:\n%s", packet, field.name, s->source);
      }
    } else {
      for (size_t m = 0; m < field.member_count; ++m) {
        FramedslField member;
        framedsl_member(s->definition, index, f, m, &member);
        if (member.has_fixed || !member.is_scalar) {
          continue;
        }
        char dotted[128];
        snprintf(dotted, sizeof(dotted), "%s.%s.%s", packet, field.name, member.name);
        FramedslFieldId any = find(s, dotted);
        size_t count;
        if (framedsl_count(s->rx, any, &count) != FRAMEDSL_OK) {
          die("%s: no element count", dotted);
        }
        for (size_t e = 0; e < count; ++e) {
          FramedslFieldId element;
          uint64_t sent, received;
          framedsl_element(s->definition, any, e, &element);
          if (framedsl_get_u64(s->tx, element, &sent) != FRAMEDSL_OK
              || framedsl_get_u64(s->rx, element, &received) != FRAMEDSL_OK || sent != received) {
            die("%s[%zu] did not round-trip:\n%s", dotted, e, s->source);
          }
        }
      }
    }
  }
}

/* The check before anything is timed: `packet` encodes to exactly `length` bytes, decodes back as
 * itself with every value intact, and the decoded frame re-encodes to the same bytes. The bytes are
 * kept for the decode operations. */
static uint8_t *check(Subject *s, const char *packet, size_t length) {
  size_t sent_length;
  FramedslStatus status;
  const uint8_t *sent = framedsl_encode(s->tx, packet, &sent_length, &status);
  if (sent == NULL) {
    die("%s did not encode (status %d):\n%s", packet, (int)status, s->source);
  }
  if (sent_length != length) {
    die("%s encoded to %zu bytes, the generator meant %zu:\n%s", packet, sent_length, length, s->source);
  }
  uint8_t *wire = allocate(length);
  memcpy(wire, sent, length);

  size_t consumed;
  const char *name = framedsl_decode(s->rx, wire, length, &consumed, &status);
  if (name == NULL || strcmp(name, packet) != 0 || consumed != length) {
    die("%s did not decode back as itself (status %d):\n%s", packet, (int)status, s->source);
  }
  compare_fields(s, packet);

  size_t again_length;
  const uint8_t *again = framedsl_encode(s->rx, packet, &again_length, NULL);
  if (again == NULL || again_length != length || memcmp(again, wire, length) != 0) {
    die("%s re-encoded differently after a decode:\n%s", packet, s->source);
  }
  return wire;
}

static void check_first(Subject *s, size_t length) {
  s->wire_first = check(s, s->first, length);
  s->length_first = length;
  s->wire_last = s->wire_first;
  s->length_last = length;
}

static void use_scalar(Subject *s, const char *dotted, unsigned bits) {
  s->scalar = find(s, dotted);
  s->scalar_mask = bits >= 64 ? UINT64_MAX : (UINT64_C(1) << bits) - 1;
}

static void use_member(Subject *s, const char *dotted, size_t elements) {
  s->has_member = true;
  s->member = find(s, dotted);
  s->elements = elements;
}

/* Sixteen engineering values whose inverse lands on raw values spread over `bits`, worked out through
 * the curve itself so every one of them is valid for framedsl_set_eng(). */
static void use_cal(Subject *s, const char *dotted, const char *curve, unsigned bits) {
  s->has_cal = true;
  s->cal = find(s, dotted);
  FramedslCalId cal;
  if (!framedsl_find_cal(s->definition, curve, &cal)) {
    die("no calibration %s", curve);
  }
  const uint64_t top = (UINT64_C(1) << bits) - 2;
  for (unsigned i = 0; i < 16; ++i) {
    if (framedsl_calibrate(s->definition, cal, (double)(1 + top * i / 16), &s->eng[i]) != FRAMEDSL_OK) {
      die("calibration %s failed", curve);
    }
  }
}

/* ---- The generators ----------------------------------------------------------------------------------
 *
 * Each dimension varies one thing, with the rest held at a small default. Every packet starts with a
 * fixed 8-bit id (0x5A), which is what a decode matches on, and is msb_first and big_endian unless the
 * point is about something else. `quick` keeps a few values of each dimension, for a run in seconds. */

static const char *const HEADER = "@bit_order: msb_first;\n@byte_order: big_endian;\n\n";

static void op_codec(Sweep *sweep, Subject *s, const char *dimension, const char *variant, uint64_t value) {
  add_point(sweep, s, dimension, variant, value, "encode", encode_first);
  add_point(sweep, s, dimension, variant, value, "decode", decode_first);
}

static void op_accessors(Sweep *sweep, Subject *s, const char *dimension, const char *variant, uint64_t value) {
  add_point(sweep, s, dimension, variant, value, "set_u64", set_u64);
  add_point(sweep, s, dimension, variant, value, "get_u64", get_u64);
}

static void op_elements(Sweep *sweep, Subject *s, const char *dimension, const char *variant, uint64_t value) {
  add_point(sweep, s, dimension, variant, value, "element_get", element_get);
  add_point(sweep, s, dimension, variant, value, "elements_all", elements_all);
}

/* Packet size, in bytes on the wire: one fixed-width bit string; the same off a byte boundary; and a
 * valof()-sized payload filled to its reservation. */
static void sweep_size(Sweep *sweep, bool quick) {
  static const uint64_t full[] = { 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096 };
  static const uint64_t fast[] = { 16, 256, 4096 };
  const uint64_t *sizes = quick ? fast : full;
  size_t n = quick ? sizeof(fast) / sizeof(*fast) : sizeof(full) / sizeof(*full);

  for (size_t k = 0; k < n; ++k) {
    uint64_t size = sizes[k];
    Text text = { 0 };
    Subject *s;

    append(&text, "%spkt P { id: 8 := 0x5A; seq: 16; data: %" PRIu64 "; };\n", HEADER, (size - 3) * 8);
    s = load(sweep, &text);
    set(s, "P.seq", 0x1234);
    set_pattern(s, "P.data", size - 3);
    check_first(s, size);
    op_codec(sweep, s, "size", "fixed", size);

    append(&text, "%spkt P { id: 8 := 0x5A; seq: 12; data: %" PRIu64 "; tail: 4; };\n", HEADER, (size - 3) * 8);
    s = load(sweep, &text);
    set(s, "P.seq", 0x123);
    set(s, "P.tail", 0x9);
    set_pattern(s, "P.data", size - 3);
    check_first(s, size);
    op_codec(sweep, s, "size", "fixed_unaligned", size);

    append(
      &text, "%spkt P { id: 8 := 0x5A; len: 16; data[%" PRIu64 "]: valof(len) * 8; crc: 8; };\n", HEADER, size - 4
    );
    s = load(sweep, &text);
    set(s, "P.len", size - 4);
    set_pattern(s, "P.data", size - 4);
    set(s, "P.crc", 0xC7);
    check_first(s, size);
    op_codec(sweep, s, "size", "valof", size);
  }
}

/* Fields per packet: N 16-bit fields after the id, up to the most a packet may have. The accessors
 * work on the one in the middle. */
static void sweep_fields(Sweep *sweep, bool quick) {
  static const uint64_t full[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 255 };
  static const uint64_t fast[] = { 1, 16, 255 };
  const uint64_t *counts = quick ? fast : full;
  size_t n = quick ? sizeof(fast) / sizeof(*fast) : sizeof(full) / sizeof(*full);

  for (size_t k = 0; k < n; ++k) {
    uint64_t count = counts[k];
    Text text = { 0 };
    append(&text, "%spkt P {\n  id: 8 := 0x5A;\n", HEADER);
    for (uint64_t f = 0; f < count; ++f) {
      append(&text, "  f%" PRIu64 ": 16;\n", f);
    }
    append(&text, "};\n");
    Subject *s = load(sweep, &text);
    for (uint64_t f = 0; f < count; ++f) {
      char dotted[32];
      snprintf(dotted, sizeof(dotted), "P.f%" PRIu64, f);
      set(s, dotted, (f * 2654435761u + 1) & 0xFFFF);
    }
    check_first(s, 1 + 2 * count);
    char middle[32];
    snprintf(middle, sizeof(middle), "P.f%" PRIu64, count / 2);
    use_scalar(s, middle, 16);
    op_codec(sweep, s, "fields", "u16", count);
    op_accessors(sweep, s, "fields", "u16", count);
  }
}

/* One block for the repetition dimensions: an element of `members`, each written as the text of one
 * field. Each element's non-fixed scalars are set to something distinct per element before the check. */
static Subject *repeated(Sweep *sweep, const char *block, bool counted, uint64_t elements, uint64_t element_bytes) {
  Text text = { 0 };
  append(&text, "%sblock E {\n%s};\n\n", HEADER, block);
  if (counted) {
    append(
      &text, "pkt P { id: 8 := 0x5A; count: 16; samples[%" PRIu64 "]: E[valof(count)]; };\n", elements * element_bytes
    );
  } else {
    append(&text, "pkt P { id: 8 := 0x5A; samples: E[%" PRIu64 "]; };\n", elements);
  }
  Subject *s = load(sweep, &text);
  if (counted) {
    set(s, "P.count", elements);
  }

  size_t packet;
  framedsl_find_packet(s->definition, "P", &packet);
  size_t field = counted ? 2 : 1;
  FramedslField info;
  framedsl_field(s->definition, packet, field, &info);
  for (size_t m = 0; m < info.member_count; ++m) {
    FramedslField member;
    framedsl_member(s->definition, packet, field, m, &member);
    if (member.has_fixed || !member.is_scalar) {
      continue;
    }
    char dotted[64];
    snprintf(dotted, sizeof(dotted), "P.samples.%s", member.name);
    FramedslFieldId any = find(s, dotted);
    uint64_t mask = member.width_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << member.width_bits) - 1;
    for (uint64_t e = 0; e < elements; ++e) {
      FramedslFieldId element;
      framedsl_element(s->definition, any, e, &element);
      if (framedsl_set_u64(s->tx, element, (e * 40503u + m * 7 + 1) & mask) != FRAMEDSL_OK) {
        die("could not set %s[%" PRIu64 "]", dotted, e);
      }
    }
  }
  check_first(s, 1 + (counted ? 2 : 0) + elements * element_bytes);
  return s;
}

/* The element bench/bench.c uses: a fixed 4-bit marker, a 4-bit channel, a little-endian 16-bit reading. */
static const char *const SAMPLE =
  "  marker: 4 := 0xA;\n  channel: 4;\n  [ @byte_order: little_endian; ] reading: 16;\n";

/* Repetitions: how many elements, with the count a constant or a field; then, at 32 counted elements,
 * how big an element is and what its members are like. */
static void sweep_repetition(Sweep *sweep, bool quick) {
  static const uint64_t full[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024 };
  static const uint64_t fast[] = { 1, 32, 1024 };
  const uint64_t *counts = quick ? fast : full;
  size_t n = quick ? sizeof(fast) / sizeof(*fast) : sizeof(full) / sizeof(*full);

  for (size_t k = 0; k < n; ++k) {
    for (int counted = 0; counted <= 1; ++counted) {
      const char *variant = counted ? "counted" : "const";
      Subject *s = repeated(sweep, SAMPLE, counted, counts[k], 3);
      use_member(s, "P.samples.reading", counts[k]);
      op_codec(sweep, s, "elements", variant, counts[k]);
      op_elements(sweep, s, "elements", variant, counts[k]);
    }
  }

  /* Element size: K whole-byte members, no fixed value, no reordering. */
  static const uint64_t sizes_full[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32 };
  static const uint64_t sizes_fast[] = { 1, 8, 32 };
  const uint64_t *sizes = quick ? sizes_fast : sizes_full;
  n = quick ? sizeof(sizes_fast) / sizeof(*sizes_fast) : sizeof(sizes_full) / sizeof(*sizes_full);
  for (size_t k = 0; k < n; ++k) {
    Text block = { 0 };
    for (uint64_t m = 0; m < sizes[k]; ++m) {
      append(&block, "  m%" PRIu64 ": 8;\n", m);
    }
    Subject *s = repeated(sweep, block.text, true, 32, sizes[k]);
    free(block.text);
    use_member(s, "P.samples.m0", 32);
    op_codec(sweep, s, "element_bytes", "u8_members", sizes[k]);
    op_elements(sweep, s, "element_bytes", "u8_members", sizes[k]);
  }

  /* What an element's members are like, at 32 counted elements of 3 bytes. `reading` is what is read. */
  static const struct {
    const char *name;
    const char *block;
  } shapes[] = {
    { "plain",               "  channel: 8;\n  reading: 16;\n"                                                       },
    { "fixed",               "  marker: 8 := 0xA5;\n  reading: 16;\n"                                                },
    { "little_endian",       "  channel: 8;\n  [ @byte_order: little_endian; ] reading: 16;\n"                       },
    { "fixed_little_endian", "  marker: 8 := 0xA5;\n  [ @byte_order: little_endian; ] reading: 16;\n"                },
    { "sub_byte_fixed",      "  marker: 4 := 0xA;\n  channel: 4;\n  reading: 16;\n"                                  },
    { "lsb_first",           "  [ @bit_order: lsb_first; ] channel: 8;\n  [ @bit_order: lsb_first; ] reading: 16;\n" },
    { "bench_sample",        NULL                                                                                    },
  };
  for (size_t k = 0; k < sizeof(shapes) / sizeof(*shapes); ++k) {
    if (quick && k != 0 && k != 1 && k != 6) {
      continue;
    }
    Subject *s = repeated(sweep, shapes[k].block != NULL ? shapes[k].block : SAMPLE, true, 32, 3);
    use_member(s, "P.samples.reading", 32);
    op_codec(sweep, s, "element_shape", shapes[k].name, 32);
    op_elements(sweep, s, "element_shape", shapes[k].name, 32);
  }
}

/* Packets per definition: P packets told apart by a 16-bit id, each two 16-bit fields. A decode tries
 * packets in turn, and an encode finds its packet by name, so the first and the last are both timed. */
static void sweep_packets(Sweep *sweep, bool quick) {
  static const uint64_t full[] = { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
  static const uint64_t fast[] = { 1, 32, 512 };
  const uint64_t *counts = quick ? fast : full;
  size_t n = quick ? sizeof(fast) / sizeof(*fast) : sizeof(full) / sizeof(*full);

  for (size_t k = 0; k < n; ++k) {
    uint64_t count = counts[k];
    Text text = { 0 };
    append(&text, "%s", HEADER);
    for (uint64_t p = 0; p < count; ++p) {
      append(&text, "pkt P%" PRIu64 " { id: 16 := %" PRIu64 "; a: 16; b: 16; };\n", p, p + 1);
    }
    Subject *s = load(sweep, &text);
    char first[32], last[32];
    snprintf(first, sizeof(first), "P%d", 0);
    snprintf(last, sizeof(last), "P%" PRIu64, count - 1);
    s->first = framedsl_packet_name(s->definition, 0);
    s->last = framedsl_packet_name(s->definition, count - 1);
    if (strcmp(s->first, first) != 0 || strcmp(s->last, last) != 0) {
      die("packets are not in declaration order");
    }
    char dotted[48];
    snprintf(dotted, sizeof(dotted), "%s.a", first);
    set(s, dotted, 0x1111);
    snprintf(dotted, sizeof(dotted), "%s.b", first);
    set(s, dotted, 0x2222);
    snprintf(dotted, sizeof(dotted), "%s.a", last);
    set(s, dotted, 0x3333);
    snprintf(dotted, sizeof(dotted), "%s.b", last);
    set(s, dotted, 0x4444);
    s->wire_first = check(s, s->first, 6);
    s->length_first = 6;
    s->wire_last = check(s, s->last, 6);
    s->length_last = 6;

    add_point(sweep, s, "packets", "first", count, "encode", encode_first);
    add_point(sweep, s, "packets", "last", count, "encode", encode_last);
    add_point(sweep, s, "packets", "first", count, "decode", decode_first);
    add_point(sweep, s, "packets", "last", count, "decode", decode_last);
  }
}

/* Field shape: eight fields of one shape after the id. The accessors work on the fifth. */
static void sweep_shape(Sweep *sweep, bool quick) {
  static const struct {
    const char *name;
    unsigned bits;
    const char *attributes; /* written in front of every field */
    const char *cal; /* a calibration applied to every field, or NULL */
  } shapes[] = {
    { "u8",                8,  "",                                 NULL     },
    { "u16",               16, "",                                 NULL     },
    { "u32",               32, "",                                 NULL     },
    { "u64",               64, "",                                 NULL     },
    { "u12_sub_byte",      12, "",                                 NULL     },
    { "u3_sub_byte",       3,  "",                                 NULL     },
    { "u16_lsb_first",     16, "[ @bit_order: lsb_first; ] ",      NULL     },
    { "u12_lsb_first",     12, "[ @bit_order: lsb_first; ] ",      NULL     },
    { "u16_little_endian", 16, "[ @byte_order: little_endian; ] ", NULL     },
    { "u32_little_endian", 32, "[ @byte_order: little_endian; ] ", NULL     },
    { "u64_little_endian", 64, "[ @byte_order: little_endian; ] ", NULL     },
    { "u16_cal_linear",    16, "[ @cal: linear; ] ",               "linear" },
    { "u16_cal_curve",     16, "[ @cal: ratio; ] ",                "ratio"  },
  };
  for (size_t k = 0; k < sizeof(shapes) / sizeof(*shapes); ++k) {
    if (quick && k != 1 && k != 4 && k != 8 && k != 12) {
      continue;
    }
    Text text = { 0 };
    append(&text, "%s", HEADER);
    append(&text, "cal linear(raw) = raw * 0.5 + 10;\n");
    append(&text, "cal ratio(raw) = 10000 * raw / (65535 - raw) inverse (eng) = 65535 * eng / (10000 + eng);\n\n");
    append(&text, "pkt P {\n  id: 8 := 0x5A;\n");
    for (unsigned f = 0; f < 8; ++f) {
      append(&text, "  %sf%u: %u;\n", shapes[k].attributes, f, shapes[k].bits);
    }
    append(&text, "};\n");
    Subject *s = load(sweep, &text);
    uint64_t mask = shapes[k].bits >= 64 ? UINT64_MAX : (UINT64_C(1) << shapes[k].bits) - 1;
    for (unsigned f = 0; f < 8; ++f) {
      char dotted[16];
      snprintf(dotted, sizeof(dotted), "P.f%u", f);
      /* Below the top of the range: 65535 is the one raw value the ratio curve divides by zero at. */
      set(s, dotted, (UINT64_C(0x9E3779B97F4A7C15) * (f + 1)) & (mask >> 1));
    }
    check_first(s, 1 + 8 * shapes[k].bits / 8);
    use_scalar(s, "P.f4", shapes[k].bits);
    /* Raw values the curve cannot take would fail set_u64 through get_eng afterwards; keep them in range. */
    if (shapes[k].cal != NULL) {
      s->scalar_mask >>= 1;
      use_cal(s, "P.f4", shapes[k].cal, shapes[k].bits);
    }
    op_codec(sweep, s, "shape", shapes[k].name, 8);
    op_accessors(sweep, s, "shape", shapes[k].name, 8);
    if (s->has_cal) {
      add_point(sweep, s, "shape", shapes[k].name, 8, "set_eng", set_eng);
      add_point(sweep, s, "shape", shapes[k].name, 8, "get_eng", get_eng);
    }
  }
}

/* ---- Combinations (#130) ---------------------------------------------------------------------------------
 *
 * The dimensions above vary one thing each, and no real definition is one of their points: a real packet
 * mixes widths, alignments, orders, calibrations, repetitions and conditional parts, and a link carries
 * a family of packets that share a header. `mix` is a handful of definitions written the way real ones
 * are; `grid` crosses a field's shape with the bit offset the fields start at and how many there are.
 * A change that speeds up one shape at another's expense shows up as such. */

/* Every field of `packet` a caller would set, set to something distinct: scalars to a value that fits
 * (below the top of the range, for the calibrations' sake), fixed-width bit strings to a pattern, and
 * the members of the first `elements` elements of each repetition. A variable-width bit string, and a
 * field the layout depends on -- a length, a count, a flag a condition reads -- are the caller's to set
 * after this, to what the point needs. */
static void fill(Subject *s, const char *packet, uint64_t elements) {
  size_t index;
  if (!framedsl_find_packet(s->definition, packet, &index)) {
    die("no packet %s", packet);
  }
  for (size_t f = 0; f < framedsl_field_count(s->definition, index); ++f) {
    FramedslField field;
    framedsl_field(s->definition, index, f, &field);
    if (field.has_fixed) {
      continue;
    }
    char dotted[128];
    snprintf(dotted, sizeof(dotted), "%s.%s", packet, field.name);
    if (field.is_scalar) {
      uint64_t mask = field.width_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << field.width_bits) - 1;
      set(s, dotted, (UINT64_C(0x9E3779B97F4A7C15) * (f + 1)) & (mask >> 1));
    } else if (field.element_bits == 0) {
      if (!field.variable) {
        set_pattern(s, dotted, (size_t)(field.width_bits / 8));
      }
    } else {
      for (size_t m = 0; m < field.member_count; ++m) {
        FramedslField member;
        framedsl_member(s->definition, index, f, m, &member);
        if (member.has_fixed || !member.is_scalar) {
          continue;
        }
        char name[192];
        snprintf(name, sizeof(name), "%s.%s", dotted, member.name);
        FramedslFieldId any = find(s, name);
        uint64_t mask = member.width_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << member.width_bits) - 1;
        for (uint64_t e = 0; e < elements; ++e) {
          FramedslFieldId element;
          framedsl_element(s->definition, any, e, &element);
          if (framedsl_set_u64(s->tx, element, (e * 40503u + m * 7 + 1) & mask) != FRAMEDSL_OK) {
            die("could not set %s[%" PRIu64 "]", name, e);
          }
        }
      }
    }
  }
}

/* A family of packets sharing a header, `length` bytes each. The first and the last declared are filled,
 * checked and timed: encode finds a packet by name and decode tells it from the others, so where a
 * packet sits in its family is part of what it costs. */
static void mix_family(Sweep *sweep, Subject *s, const char *variant, size_t length) {
  const size_t count = framedsl_packet_count(s->definition);
  s->first = framedsl_packet_name(s->definition, 0);
  s->last = framedsl_packet_name(s->definition, count - 1);
  fill(s, s->first, 0);
  fill(s, s->last, 0);
  s->wire_first = check(s, s->first, length);
  s->length_first = length;
  s->wire_last = check(s, s->last, length);
  s->length_last = length;

  char name[32];
  snprintf(name, sizeof(name), "%s_first", variant);
  add_point(sweep, s, "mix", name, length, "encode", encode_first);
  add_point(sweep, s, "mix", name, length, "decode", decode_first);
  snprintf(name, sizeof(name), "%s_last", variant);
  add_point(sweep, s, "mix", name, length, "encode", encode_last);
  add_point(sweep, s, "mix", name, length, "decode", decode_last);
}

/* A CCSDS space packet family: the primary header's sub-byte fields, three of them across a byte
 * boundary, and 32 application ids. Each packet's data field is laid out a little differently, as one
 * instrument's would be from the next, and is 23 bytes in all of them. */
static void mix_ccsds(Sweep *sweep) {
  Text text = { 0 };
  append(&text, "%s", HEADER);
  for (unsigned p = 0; p < 32; ++p) {
    unsigned k = p % 3;
    append(
      &text,
      "pkt TM_APID_%u {\n"
      "  version: 3 := 0; type: 1 := 0; sec_hdr: 1; apid: 11 := %u;\n"
      "  seq_flags: 2; seq_count: 14; data_length: 16;\n"
      "  a: %u; b: 16; c: 32; samples: %u;\n"
      "};\n",
      p,
      0x100 + p,
      8 + 8 * k,
      128 - 8 * k
    );
  }
  mix_family(sweep, load(sweep, &text), "ccsds_family", 6 + 23);
}

/* Telecommands told apart by a subtype that is not the first field: every one has the same type byte,
 * so the fixed value that identifies a packet sits three bytes in. 16 of them, with argument layouts
 * that differ but all take four bytes. */
static void mix_telecommands(Sweep *sweep) {
  static const char *const arguments[] = {
    "a: 32;",
    "a: 8; b: 8; c: 16;",
    "a: 4; b: 12; c: 16;",
    "[ @byte_order: little_endian; ] a: 32;",
  };
  Text text = { 0 };
  append(&text, "%s", HEADER);
  for (unsigned p = 0; p < 16; ++p) {
    append(&text, "pkt TC_%u { type: 8 := 0x1C; seq: 16; subtype: 8 := %u; %s };\n", p, 0x40 + p, arguments[p % 4]);
  }
  mix_family(sweep, load(sweep, &text), "telecommands", 8);
}

/* One packet: filled, with the layout fields then set as `layout` says, checked to be `length` bytes,
 * and its encode and decode timed. */
static Subject *mix_single(Sweep *sweep, Text *text, const char *variant, size_t length, void (*layout)(Subject *)) {
  Subject *s = load(sweep, text);
  fill(s, s->first, 32);
  if (layout != NULL) {
    layout(s);
  }
  check_first(s, length);
  op_codec(sweep, s, "mix", variant, length);
  return s;
}

/* Housekeeping: an enum, sub-byte flags, little-endian calibrated temperatures, 12-bit calibrated
 * voltages that cross byte boundaries, a little-endian uptime and a 64-bit time. 29 bytes. */
static void mix_housekeeping(Sweep *sweep) {
  Text text = { 0 };
  append(&text, "%s", HEADER);
  append(&text, "cal temp(raw) = raw * 0.01 - 40;\ncal volts(raw) = raw * 3.3 / 4095;\n\n");
  append(&text, "enum Mode: 3 { SAFE; NOMINAL; SCIENCE; };\n\n");
  append(&text, "pkt TM_HK {\n  id: 8 := 0x5A;\n  mode: Mode;\n  flags: 5;\n");
  for (unsigned t = 0; t < 4; ++t) {
    append(&text, "  [ @byte_order: little_endian; @cal: temp; ] t%u: 16;\n", t);
  }
  for (unsigned v = 0; v < 4; ++v) {
    append(&text, "  [ @cal: volts; ] v%u: 12;\n", v);
  }
  append(&text, "  [ @byte_order: little_endian; ] uptime: 32;\n  time: 64;\n  status: 8;\n};\n");
  Subject *s = mix_single(sweep, &text, "housekeeping", 29, NULL);
  use_scalar(s, "TM_HK.v2", 12);
  use_cal(s, "TM_HK.t1", "temp", 16);
  s->scalar_mask >>= 1;
  add_point(sweep, s, "mix", "housekeeping", 29, "get_u64", get_u64);
  add_point(sweep, s, "mix", "housekeeping", 29, "get_eng", get_eng);
  add_point(sweep, s, "mix", "housekeeping", 29, "set_eng", set_eng);
}

/* lsb_first readings off a byte boundary, followed by a 64-byte bit string three bits into a byte. */
static void mix_unaligned_lsb_first(Sweep *sweep) {
  Text text = { 0 };
  append(&text, "%spkt P {\n  id: 8 := 0x5A;\n  seq: 3;\n", HEADER);
  for (unsigned f = 0; f < 4; ++f) {
    append(&text, "  [ @bit_order: lsb_first; ] r%u: 16;\n", f);
  }
  append(&text, "  data: %u;\n  tail: 5;\n};\n", 64 * 8);
  Subject *s = mix_single(sweep, &text, "unaligned_lsb_first", 74, NULL);
  use_scalar(s, "P.r2", 16);
  add_point(sweep, s, "mix", "unaligned_lsb_first", 74, "get_u64", get_u64);
}

static void counted_32(Subject *s) {
  set(s, "P.count", 32);
}

/* A counted repetition of 32 six-byte elements whose members are of every kind: a fixed sub-byte marker,
 * a little-endian reading, an lsb_first status, a 12-bit value across a byte boundary. 198 bytes. */
static void mix_repetition(Sweep *sweep) {
  Text text = { 0 };
  append(
    &text,
    "%sblock E {\n"
    "  marker: 4 := 0xA;\n  channel: 4;\n"
    "  [ @byte_order: little_endian; ] reading: 16;\n"
    "  [ @bit_order: lsb_first; ] status: 8;\n"
    "  value: 12;\n  spare: 4;\n"
    "};\n\n"
    "pkt P { id: 8 := 0x5A; seq: 16; count: 8; samples[192]: E[valof(count)]; crc: 16; };\n",
    HEADER
  );
  Subject *s = mix_single(sweep, &text, "repetition", 198, counted_32);
  use_member(s, "P.samples.reading", 32);
  op_elements(sweep, s, "mix", "repetition", 198);
}

static const char *const CONDITIONAL =
  "enum Kind: 4 { IDLE; HK; EVENT; };\n\n"
  "block Time { seconds: 32; subsecs: 16; };\n"
  "block R { channel: 8; value: 16; };\n\n"
  "pkt P {\n"
  "  id: 8 := 0x5A;\n"
  "  sec_hdr: 1; has_crc: 1; spare: 2; kind: Kind;\n"
  "  [ @if: valof(sec_hdr) == 1; ] time: Time;\n"
  "  [ @if: valof(kind) != IDLE; ] block {\n"
  "    count: 8;\n"
  "    readings[24]: R[valof(count)];\n"
  "    [ @if: valof(kind) == EVENT; ] code: 16;\n"
  "  };\n"
  "  [ @if: valof(has_crc) == 1; ] crc: 16;\n"
  "};\n";

static void conditional_all(Subject *s) {
  set(s, "P.sec_hdr", 1);
  set(s, "P.has_crc", 1);
  set(s, "P.kind", 2);
  set(s, "P.count", 8);
}

static void conditional_none(Subject *s) {
  set(s, "P.sec_hdr", 0);
  set(s, "P.has_crc", 0);
  set(s, "P.kind", 0);
}

/* Optional parts: a secondary header, a counted block present unless the kind is IDLE with a code inside
 * it only for an EVENT, and a CRC. Every part present (37 bytes), and none (2). */
static void mix_conditional(Sweep *sweep, bool quick) {
  Text text = { 0 };
  append(&text, "%s%s", HEADER, CONDITIONAL);
  mix_single(sweep, &text, "conditional_all", 37, conditional_all);
  if (!quick) {
    append(&text, "%s%s", HEADER, CONDITIONAL);
    mix_single(sweep, &text, "conditional_none", 2, conditional_none);
  }
}

static void payload_200(Subject *s) {
  set(s, "P.len", 200);
  set_pattern(s, "P.data", 200);
}

/* A valof() payload of 200 bytes that starts five bits into a byte. 203 bytes. */
static void mix_valof_unaligned(Sweep *sweep) {
  Text text = { 0 };
  append(&text, "%spkt P { id: 8 := 0x5A; len: 11; flags: 2; data[256]: valof(len) * 8; tail: 3; };\n", HEADER);
  mix_single(sweep, &text, "valof_unaligned", 203, payload_200);
}

/* 63 fields after the id, nine shapes over and over: whole bytes, sub-byte and across a boundary,
 * little_endian, lsb_first and 64 bits. 148 bytes. */
static void mix_wide(Sweep *sweep) {
  static const char *const cycle[] = {
    "%s8: 8;",
    "%s16: 16;",
    "%s3: 3;",
    "%s13: 13;",
    "[ @byte_order: little_endian; ] %s32: 32;",
    "[ @bit_order: lsb_first; ] %s16l: 16;",
    "%s5: 5;",
    "%s11: 11;",
    "%s64: 64;",
  };
  Text text = { 0 };
  append(&text, "%spkt P {\n  id: 8 := 0x5A;\n", HEADER);
  for (unsigned c = 0; c < 7; ++c) {
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "c%u_", c);
    for (size_t k = 0; k < sizeof(cycle) / sizeof(*cycle); ++k) {
      append(&text, "  ");
      append(&text, cycle[k], prefix);
      append(&text, "\n");
    }
  }
  append(&text, "};\n");
  Subject *s = mix_single(sweep, &text, "wide", 148, NULL);
  use_scalar(s, "P.c3_13", 13);
  add_point(sweep, s, "mix", "wide", 148, "get_u64", get_u64);
}

static void sweep_mix(Sweep *sweep, bool quick) {
  mix_ccsds(sweep);
  mix_repetition(sweep);
  mix_conditional(sweep, quick);
  if (quick) {
    return;
  }
  mix_telecommands(sweep);
  mix_housekeeping(sweep);
  mix_unaligned_lsb_first(sweep);
  mix_valof_unaligned(sweep);
  mix_wide(sweep);
}

/* A field's shape, crossed with the bit offset the fields start at and how many there are. Off a byte
 * boundary a little_endian field is refused (check 10), so that shape is only at offset 0. */
static void sweep_grid(Sweep *sweep, bool quick) {
  static const struct {
    const char *name;
    unsigned bits;
    const char *attributes;
  } shapes[] = {
    { "u16",               16, ""                                 },
    { "u12",               12, ""                                 },
    { "u16_lsb_first",     16, "[ @bit_order: lsb_first; ] "      },
    { "u32_little_endian", 32, "[ @byte_order: little_endian; ] " },
    { "u16_cal",           16, "[ @cal: linear; ] "               },
  };
  static const unsigned offsets[] = { 0, 3 };
  static const unsigned full[] = { 8, 64 };
  static const unsigned fast[] = { 64 };
  const unsigned *counts = quick ? fast : full;
  size_t n = quick ? sizeof(fast) / sizeof(*fast) : sizeof(full) / sizeof(*full);

  for (size_t k = 0; k < sizeof(shapes) / sizeof(*shapes); ++k) {
    if (quick && k != 0 && k != 2) {
      continue;
    }
    for (size_t o = 0; o < sizeof(offsets) / sizeof(*offsets); ++o) {
      unsigned offset = offsets[o];
      if (offset != 0 && strstr(shapes[k].attributes, "little_endian") != NULL) {
        continue;
      }
      for (size_t c = 0; c < n; ++c) {
        unsigned count = counts[c];
        unsigned bits = 8 + offset + count * shapes[k].bits;
        unsigned pad = (8 - bits % 8) % 8;
        Text text = { 0 };
        append(&text, "%scal linear(raw) = raw * 0.5 + 10;\n\npkt P {\n  id: 8 := 0x5A;\n", HEADER);
        if (offset != 0) {
          append(&text, "  pre: %u;\n", offset);
        }
        for (unsigned f = 0; f < count; ++f) {
          append(&text, "  %sf%u: %u;\n", shapes[k].attributes, f, shapes[k].bits);
        }
        if (pad != 0) {
          append(&text, "  pad: %u;\n", pad);
        }
        append(&text, "};\n");
        Subject *s = load(sweep, &text);
        fill(s, s->first, 0);
        check_first(s, (bits + pad) / 8);
        char variant[32];
        snprintf(variant, sizeof(variant), "%s_at%u", shapes[k].name, offset);
        op_codec(sweep, s, "grid", variant, count);
      }
    }
  }
}

/* ---- Timing ---------------------------------------------------------------------------------------------- */

/* Runs `iterations` of a point, and returns the seconds it took. */
static double run_batch(Point *point, uint64_t iterations) {
  int failed = 0;
  double start = now();
  for (uint64_t i = 0; i < iterations; ++i) {
    failed |= point->run(point->subject, i);
  }
  double elapsed = now() - start;
  if (failed) {
    die(
      "%s/%s/%" PRIu64 "/%s: a call failed while being timed",
      point->dimension,
      point->variant,
      point->value,
      point->operation
    );
  }
  return elapsed;
}

/* The iteration count for one sample: doubled until a batch takes the target time. */
static void calibrate(Point *point, double target) {
  uint64_t iterations = 1;
  for (;;) {
    double elapsed = run_batch(point, iterations);
    if (elapsed >= target || iterations >= (UINT64_C(1) << 40)) {
      break;
    }
    iterations *= 2;
  }
  point->iterations = iterations;
}

static int compare_doubles(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/* Nearest-rank percentile of sorted samples. */
static double percentile(const double *sorted, size_t count, double p) {
  size_t rank = (size_t)(p / 100.0 * (double)count + 0.5);
  if (rank < 1) {
    rank = 1;
  }
  if (rank > count) {
    rank = count;
  }
  return sorted[rank - 1];
}

/* A small, seeded generator for shuffling the order of each round: reproducible from run to run. */
static uint64_t next_random(uint64_t *state) {
  *state ^= *state << 13;
  *state ^= *state >> 7;
  *state ^= *state << 17;
  return *state;
}

/* ---- The machine ---------------------------------------------------------------------------------------- */

static void read_line(const char *path, char *out, size_t capacity) {
  snprintf(out, capacity, "unknown");
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    return;
  }
  if (fgets(out, (int)capacity, file) != NULL) {
    out[strcspn(out, "\n")] = '\0';
  }
  fclose(file);
}

static void cpu_model(char *out, size_t capacity) {
  snprintf(out, capacity, "unknown");
  FILE *file = fopen("/proc/cpuinfo", "r");
  if (file == NULL) {
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), file) != NULL) {
    if (strncmp(line, "model name", 10) == 0) {
      char *colon = strchr(line, ':');
      if (colon != NULL) {
        snprintf(out, capacity, "%s", colon + 2);
        out[strcspn(out, "\n")] = '\0';
      }
      break;
    }
  }
  fclose(file);
}

/* Pins the process to `cpu`, or to the last CPU it may run on when `cpu` is negative. Returns the CPU. */
static int pin(int cpu) {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    return -1;
  }
  if (cpu < 0) {
    for (int c = CPU_SETSIZE - 1; c >= 0; --c) {
      if (CPU_ISSET(c, &allowed)) {
        cpu = c;
        break;
      }
    }
  }
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(cpu, &one);
  if (sched_setaffinity(0, sizeof(one), &one) != 0) {
    return -1;
  }
  return cpu;
}

/* ---- Main ------------------------------------------------------------------------------------------------- */

static void usage(FILE *out) {
  fputs(
    "usage: baseline [--quick] [--out FILE] [--processes N] [--samples N] [--sample-ms MS] [--cpu N]\n"
    "                [--commit NAME] [--only DIMENSION] [--dump DIMENSION]\n"
    "\n"
    "Sweeps encode, decode and field access over generated definitions (#102). Writes one CSV row per\n"
    "point to FILE (default: build/performance/baseline.csv) and a summary to stdout.\n"
    "\n"
    "  --quick         a few values per dimension, 3 processes of 7 samples: seconds, not minutes\n"
    "  --processes N   separate processes the samples are taken in (default 20)\n"
    "  --samples N     samples per point in each process (default 15)\n"
    "  --sample-ms MS  the time one sample's batch is sized to take (default 2)\n"
    "  --cpu N         the CPU to pin to (default: the last one this process may use)\n"
    "  --commit NAME   what to record as the commit (make passes `git describe --always --dirty`)\n"
    "  --only DIM      sweep one dimension: size, fields, elements, element_bytes, element_shape,\n"
    "                  packets, shape, mix or grid\n"
    "  --dump DIM      print the generated definitions of one dimension, and time nothing\n",
    out
  );
}

typedef struct {
  bool quick;
  const char *out_path;
  const char *commit;
  const char *only;
  const char *dump;
  long processes;
  long samples;
  double sample_ms;
  int cpu;
  /* Set in a child: where to write its raw samples, and which child it is. */
  const char *raw;
  long child;
} Options;

/* Generates every point the options select, checking each definition as it goes. */
static void generate(Sweep *sweep, const Options *options) {
  static const struct {
    const char *name;
    void (*generate)(Sweep *, bool);
  } dimensions[] = {
    { "size",     sweep_size       },
    { "fields",   sweep_fields     },
    { "elements", sweep_repetition },
    { "packets",  sweep_packets    },
    { "shape",    sweep_shape      },
    { "mix",      sweep_mix        },
    { "grid",     sweep_grid       },
  };
  const char *selected = options->dump != NULL ? options->dump : options->only;
  bool matched = selected == NULL;
  for (size_t d = 0; d < sizeof(dimensions) / sizeof(*dimensions); ++d) {
    /* element_bytes and element_shape come out of the repetition generator. */
    bool wanted = selected == NULL || strcmp(selected, dimensions[d].name) == 0 ||
                  (dimensions[d].generate == sweep_repetition &&
                   (strcmp(selected, "element_bytes") == 0 || strcmp(selected, "element_shape") == 0));
    if (wanted) {
      matched = true;
      dimensions[d].generate(sweep, options->quick);
    }
  }
  if (!matched) {
    die("no dimension %s", selected);
  }
  /* --only element_bytes and the like: keep that dimension's points and drop the generator's others. */
  if (options->only != NULL) {
    size_t kept = 0;
    for (size_t p = 0; p < sweep->count; ++p) {
      if (strcmp(sweep->points[p].dimension, options->only) == 0) {
        sweep->points[kept++] = sweep->points[p];
      }
    }
    sweep->count = kept;
  }
}

/* One process's share: calibrate every point, then take `samples` rounds over all of them, and write
 * each point's iteration count and samples to `raw`, one line per point in generation order. */
static void measure(Sweep *sweep, const Options *options) {
  const double target = options->sample_ms / 1000.0;
  for (size_t p = 0; p < sweep->count; ++p) {
    sweep->points[p].samples = allocate((size_t)options->samples * sizeof(double));
    calibrate(&sweep->points[p], target);
  }

  size_t *order = allocate(sweep->count * sizeof(*order));
  for (size_t p = 0; p < sweep->count; ++p) {
    order[p] = p;
  }
  uint64_t seed = UINT64_C(0x2545F4914F6CDD1D) ^ (uint64_t)(options->child + 1) * UINT64_C(0x9E3779B97F4A7C15);
  for (long round = 0; round < options->samples; ++round) {
    for (size_t p = sweep->count; p > 1; --p) {
      size_t j = (size_t)(next_random(&seed) % p);
      size_t t = order[p - 1];
      order[p - 1] = order[j];
      order[j] = t;
    }
    for (size_t k = 0; k < sweep->count; ++k) {
      Point *point = &sweep->points[order[k]];
      /* A tenth of a batch untimed first: the previous point left other code and data in the caches. */
      run_batch(point, point->iterations / 10 + 1);
      point->samples[round] = run_batch(point, point->iterations) * 1e9 / (double)point->iterations;
    }
  }
  free(order);

  FILE *raw = fopen(options->raw, "w");
  if (raw == NULL) {
    die("cannot write %s", options->raw);
  }
  for (size_t p = 0; p < sweep->count; ++p) {
    fprintf(raw, "%" PRIu64, sweep->points[p].iterations);
    for (long k = 0; k < options->samples; ++k) {
      fprintf(raw, " %.4f", sweep->points[p].samples[k]);
    }
    fputc('\n', raw);
  }
  if (fclose(raw) != 0) {
    die("cannot write %s", options->raw);
  }
}

/* Runs this program again as child `child`, and waits for it. A new process rather than a loop, for
 * the reason the whole arrangement exists: exec() lays the program's stack, heap and libraries out at
 * new random addresses (ASLR), and that alone moved one point's median by up to 30% from one run to
 * the next on the machine in docs/performance.md. Samples from one process cannot see that; samples
 * pooled from many carry it in their spread, which is where it belongs. */
static void spawn(const Options *options, long child, const char *raw) {
  char child_arg[32], samples[32], sample_ms[32], cpu[32];
  snprintf(child_arg, sizeof(child_arg), "%ld", child);
  snprintf(samples, sizeof(samples), "%ld", options->samples);
  snprintf(sample_ms, sizeof(sample_ms), "%.6f", options->sample_ms);
  snprintf(cpu, sizeof(cpu), "%d", options->cpu);
  char *argv[16];
  int n = 0;
  argv[n++] = "baseline";
  argv[n++] = "--child";
  argv[n++] = child_arg;
  argv[n++] = "--raw";
  argv[n++] = (char *)raw;
  argv[n++] = "--samples";
  argv[n++] = samples;
  argv[n++] = "--sample-ms";
  argv[n++] = sample_ms;
  argv[n++] = "--cpu";
  argv[n++] = cpu;
  if (options->quick) {
    argv[n++] = "--quick";
  }
  if (options->only != NULL) {
    argv[n++] = "--only";
    argv[n++] = (char *)options->only;
  }
  argv[n] = NULL;

  pid_t pid = fork();
  if (pid < 0) {
    die("cannot fork");
  }
  if (pid == 0) {
    execv("/proc/self/exe", argv);
    _exit(127);
  }
  int status;
  if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    die("measuring process %ld failed", child);
  }
}

/* Reads child `child`'s raw samples into every point, after the samples of the children before it. */
static void collect(Sweep *sweep, const Options *options, long child, const char *raw) {
  FILE *file = fopen(raw, "r");
  if (file == NULL) {
    die("cannot read %s", raw);
  }
  for (size_t p = 0; p < sweep->count; ++p) {
    Point *point = &sweep->points[p];
    uint64_t iterations;
    if (fscanf(file, "%" SCNu64, &iterations) != 1) {
      die("%s is short", raw);
    }
    /* The smallest batch any process chose, so the column says the least a sample averaged over. */
    if (child == 0 || iterations < point->iterations) {
      point->iterations = iterations;
    }
    for (long k = 0; k < options->samples; ++k) {
      if (fscanf(file, "%lf", &point->samples[child * options->samples + k]) != 1) {
        die("%s is short", raw);
      }
    }
  }
  fclose(file);
  remove(raw);
}

int main(int argc, char **argv) {
  Options options = {
    .out_path = "build/performance/baseline.csv",
    .commit = "unknown",
    .processes = -1,
    .samples = -1,
    .sample_ms = 2.0,
    .cpu = -1,
    .child = -1,
  };

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    bool has_value = i + 1 < argc;
    if (strcmp(arg, "--quick") == 0) {
      options.quick = true;
    } else if (strcmp(arg, "--out") == 0 && has_value) {
      options.out_path = argv[++i];
    } else if (strcmp(arg, "--processes") == 0 && has_value) {
      options.processes = strtol(argv[++i], NULL, 10);
    } else if (strcmp(arg, "--samples") == 0 && has_value) {
      options.samples = strtol(argv[++i], NULL, 10);
    } else if (strcmp(arg, "--sample-ms") == 0 && has_value) {
      options.sample_ms = strtod(argv[++i], NULL);
    } else if (strcmp(arg, "--cpu") == 0 && has_value) {
      options.cpu = (int)strtol(argv[++i], NULL, 10);
    } else if (strcmp(arg, "--commit") == 0 && has_value) {
      options.commit = argv[++i];
    } else if (strcmp(arg, "--only") == 0 && has_value) {
      options.only = argv[++i];
    } else if (strcmp(arg, "--dump") == 0 && has_value) {
      options.dump = argv[++i];
    } else if (strcmp(arg, "--child") == 0 && has_value) {
      options.child = strtol(argv[++i], NULL, 10);
    } else if (strcmp(arg, "--raw") == 0 && has_value) {
      options.raw = argv[++i];
    } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      usage(stdout);
      return 0;
    } else {
      usage(stderr);
      return 2;
    }
  }
  if (options.processes < 0) {
    options.processes = options.quick ? 3 : 20;
  }
  if (options.samples < 0) {
    options.samples = options.quick ? 7 : 15;
  }
  if (options.processes < 1 || options.samples < 1 || options.sample_ms <= 0) {
    die("--processes, --samples and --sample-ms must be positive");
  }

  Sweep sweep = { 0 };
  if (options.child >= 0) {
    if (options.raw == NULL) {
      die("--child needs --raw");
    }
    pin(options.cpu);
    generate(&sweep, &options);
    measure(&sweep, &options);
    return 0;
  }

  double started = now();
  generate(&sweep, &options);
  if (options.dump != NULL) {
    for (size_t s = 0; s < sweep.subject_count; ++s) {
      printf("# ---- subject %zu\n%s\n", s, sweep.subjects[s]->source);
    }
    return 0;
  }

  /* Pinned here, and each child to the same CPU: the parent's choice is the one recorded. */
  options.cpu = pin(options.cpu);
  char governor_path[128], governor[64], model[256];
  snprintf(
    governor_path,
    sizeof(governor_path),
    "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor",
    options.cpu < 0 ? 0 : options.cpu
  );
  read_line(governor_path, governor, sizeof(governor));
  cpu_model(model, sizeof(model));
  struct utsname host;
  uname(&host);
  if (strcmp(governor, "performance") != 0) {
    fprintf(
      stderr, "baseline: the governor on cpu %d is '%s', not 'performance': expect more spread\n", options.cpu, governor
    );
  }

  const long total = options.processes * options.samples;
  fprintf(
    stderr,
    "baseline: %zu points over %zu generated definitions, all checked; %ld processes x %ld samples of ~%.1f "
    "ms\n",
    sweep.count,
    sweep.subject_count,
    options.processes,
    options.samples,
    options.sample_ms
  );
  for (size_t p = 0; p < sweep.count; ++p) {
    sweep.points[p].samples = allocate((size_t)total * sizeof(double));
  }
  char raw[4096];
  snprintf(raw, sizeof(raw), "%s.raw", options.out_path);
  for (long child = 0; child < options.processes; ++child) {
    fprintf(stderr, "\rbaseline: process %ld of %ld, %.0f s so far", child + 1, options.processes, now() - started);
    spawn(&options, child, raw);
    collect(&sweep, &options, child, raw);
  }
  fputc('\n', stderr);
  double elapsed = now() - started;

  FILE *out = fopen(options.out_path, "w");
  if (out == NULL) {
    die("cannot write %s", options.out_path);
  }
  time_t today = time(NULL);
  char date[32];
  strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S%z", localtime(&today));
  fprintf(out, "# Frame-DSL performance baseline (#102). Compare two with scripts/perf_compare.py.\n");
  fprintf(out, "# date: %s\n", date);
  fprintf(out, "# commit: %s\n", options.commit);
  fprintf(out, "# cpu: %s\n", model);
  fprintf(out, "# pinned_cpu: %d\n", options.cpu);
  fprintf(out, "# governor: %s\n", governor);
  fprintf(out, "# kernel: %s %s %s\n", host.sysname, host.release, host.machine);
  fprintf(out, "# compiler: %s %s\n", BASELINE_CC, __VERSION__);
  fprintf(out, "# cflags: %s\n", BASELINE_CFLAGS);
  fprintf(out, "# mode: %s\n", options.quick ? "quick" : "full");
  fprintf(out, "# processes: %ld\n", options.processes);
  fprintf(out, "# samples_per_process: %ld\n", options.samples);
  fprintf(out, "# sample_ms: %.3f\n", options.sample_ms);
  fprintf(out, "# seconds: %.1f\n", elapsed);
  fprintf(out, "dimension,variant,value,operation,samples,iterations,median_ns,min_ns,p10_ns,p90_ns,max_ns\n");

  printf(
    "%-14s %-20s %6s %-13s %10s %10s %10s\n", "dimension", "variant", "value", "operation", "median ns", "p10", "p90"
  );
  for (size_t p = 0; p < sweep.count; ++p) {
    Point *point = &sweep.points[p];
    qsort(point->samples, (size_t)total, sizeof(double), compare_doubles);
    double median = percentile(point->samples, (size_t)total, 50);
    double p10 = percentile(point->samples, (size_t)total, 10);
    double p90 = percentile(point->samples, (size_t)total, 90);
    fprintf(
      out,
      "%s,%s,%" PRIu64 ",%s,%ld,%" PRIu64 ",%.2f,%.2f,%.2f,%.2f,%.2f\n",
      point->dimension,
      point->variant,
      point->value,
      point->operation,
      total,
      point->iterations,
      median,
      point->samples[0],
      p10,
      p90,
      point->samples[total - 1]
    );
    printf(
      "%-14s %-20s %6" PRIu64 " %-13s %10.1f %10.1f %10.1f\n",
      point->dimension,
      point->variant,
      point->value,
      point->operation,
      median,
      p10,
      p90
    );
  }
  if (fclose(out) != 0) {
    die("cannot write %s", options.out_path);
  }

  fprintf(stderr, "baseline: %zu points in %.1f s, written to %s\n", sweep.count, elapsed, options.out_path);
  return 0;
}
