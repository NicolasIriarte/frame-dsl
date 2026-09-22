/* How fast encoding, decoding and calibration are, measured the way a program uses them.
 *
 * Written against include/framedsl.h alone and linked against build/libframedsl.a, like
 * examples/uart_device.c: what is measured is what a caller gets, not an internal loop.
 *
 * Each measurement doubles its iteration count until one batch has run for at least the minimum time
 * (0.25 s, or the first argument, in seconds), then reports that batch. Not a test: nothing here
 * fails on a slow result, because timing on a shared machine is too noisy to fail a build on. The
 * numbers are to look at, and to compare before and after a change on the same machine. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "framedsl.h"

/* Every packet a measurement uses. One file, so each measurement is one frame over one definition,
 * and the packets cover the shapes the runtime treats differently: whole bytes, sub-byte fields with
 * a little-endian one among them, a valof()-sized payload, and a packet built from blocks. */
static const char *const DEFINITION =
  "@bit_order: msb_first;\n"
  "@byte_order: big_endian;\n"
  "\n"
  "cal volts(raw) = raw * 3.3 / 4095;\n"
  "cal ohms(raw)  = 10000 * raw / (4095 - raw) inverse (eng) = 4095 * eng / (10000 + eng);\n"
  "\n"
  "block Header  { id: 8; sequence: 16; };\n"
  "block Vector3 { x: 16; y: 16; z: 16; };\n"
  "\n"
  "pkt TC_SET_LIMIT { id: 8 = 0x02; sequence: 16; channel: 8; limit: 32; };\n"
  "\n"
  "pkt TM_BITS {\n"
  "  id: 8 = 0x10; version: 3; type: 1; flag: 1; apid: 11; seq_flags: 2; seq_count: 14;\n"
  "  [ @byte_order: little_endian; ] counter: 32;\n"
  "};\n"
  "\n"
  "pkt TM_PAYLOAD { id: 8 = 0x03; len: 16; data[256]: valof(len) * 8; };\n"
  "\n"
  "pkt TM_IMU {\n"
  "  hdr: Header { id = 0x83; };\n"
  "  accel: Vector3; gyro: Vector3;\n"
  "  [ @cal: volts; ] bus: 12; [ @cal: ohms; ] sensor: 12;\n"
  "};\n";

/* Written to so the compiler cannot conclude a result is unused. The library is a separate archive
 * with no link-time optimisation, so the calls could not be removed anyway; this makes it certain. */
static volatile uint64_t sink;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

typedef struct {
  FramedslFrame *frame;
  const FramedslDefinition *definition;

  /* Resolved once, as a program would. */
  FramedslFieldId limit;
  FramedslFieldId counter;
  FramedslFieldId len;
  FramedslFieldId data;
  FramedslFieldId gyro_z;
  FramedslFieldId bus;
  FramedslFieldId sensor;

  /* Each packet's encoded bytes, captured once, for the decode measurements to read. */
  uint8_t set_limit[16];
  size_t set_limit_length;
  uint8_t bits[16];
  size_t bits_length;
  uint8_t payload[512];
  size_t payload_length;
  uint8_t imu[64];
  size_t imu_length;
} Bench;

typedef void (*Body)(Bench *bench, uint64_t iteration);

/* ---- The bodies: one operation each, as a program would do it ---------------------------------- */

static void encode_set_limit(Bench *bench, uint64_t i) {
  size_t length;
  framedsl_set_u64(bench->frame, bench->limit, i & 0xFFFFFFFF);
  const uint8_t *bytes = framedsl_encode(bench->frame, "TC_SET_LIMIT", &length, NULL);
  sink += bytes[length - 1];
}

static void decode_set_limit(Bench *bench, uint64_t i) {
  (void)i;
  size_t consumed;
  sink += (uintptr_t)framedsl_decode(bench->frame, bench->set_limit, bench->set_limit_length, &consumed, NULL);
}

static void encode_bits(Bench *bench, uint64_t i) {
  size_t length;
  framedsl_set_u64(bench->frame, bench->counter, i & 0xFFFFFFFF);
  const uint8_t *bytes = framedsl_encode(bench->frame, "TM_BITS", &length, NULL);
  sink += bytes[length - 1];
}

static void decode_bits(Bench *bench, uint64_t i) {
  (void)i;
  size_t consumed;
  sink += (uintptr_t)framedsl_decode(bench->frame, bench->bits, bench->bits_length, &consumed, NULL);
}

static void encode_payload(Bench *bench, uint64_t i) {
  size_t length;
  (void)i;
  /* The length must agree with the bytes set, or the encode is refused -- and a refusal is not what is
   * being measured. */
  const uint8_t *bytes = framedsl_encode(bench->frame, "TM_PAYLOAD", &length, NULL);
  sink += bytes[length - 1];
}

static void decode_payload(Bench *bench, uint64_t i) {
  (void)i;
  size_t consumed;
  sink += (uintptr_t)framedsl_decode(bench->frame, bench->payload, bench->payload_length, &consumed, NULL);
}

static void encode_imu(Bench *bench, uint64_t i) {
  size_t length;
  framedsl_set_u64(bench->frame, bench->gyro_z, i & 0xFFFF);
  const uint8_t *bytes = framedsl_encode(bench->frame, "TM_IMU", &length, NULL);
  sink += bytes[length - 1];
}

static void decode_imu(Bench *bench, uint64_t i) {
  (void)i;
  size_t consumed;
  sink += (uintptr_t)framedsl_decode(bench->frame, bench->imu, bench->imu_length, &consumed, NULL);
}

static void read_linear(Bench *bench, uint64_t i) {
  (void)i;
  double volts;
  framedsl_get_eng(bench->frame, bench->bus, &volts);
  sink += (uint64_t)volts;
}

static void read_curve(Bench *bench, uint64_t i) {
  (void)i;
  double ohms;
  framedsl_get_eng(bench->frame, bench->sensor, &ohms);
  sink += (uint64_t)ohms;
}

static void write_linear(Bench *bench, uint64_t i) {
  framedsl_set_eng(bench->frame, bench->bus, (double)(i & 3) * 0.5);
}

static void write_curve(Bench *bench, uint64_t i) {
  framedsl_set_eng(bench->frame, bench->sensor, 1000.0 + (double)(i & 7));
}

/* ---- Running ------------------------------------------------------------------------------------ */

static void measure(Bench *bench, const char *name, Body body, double minimum) {
  uint64_t iterations = 1024;
  double elapsed = 0;

  for (;;) {
    double start = now();
    for (uint64_t i = 0; i < iterations; ++i) {
      body(bench, i);
    }
    elapsed = now() - start;
    if (elapsed >= minimum || iterations >= (UINT64_C(1) << 40)) {
      break;
    }
    iterations *= 2;
  }

  const double ns = elapsed * 1e9 / (double)iterations;
  printf("%-34s %9.1f ns   %12.0f /s   (%llu iterations)\n", name, ns, 1e9 / ns, (unsigned long long)iterations);
}

static bool resolve(Bench *bench, const char *dotted, FramedslFieldId *id) {
  if (!framedsl_find(bench->definition, dotted, id)) {
    fprintf(stderr, "bench: no field %s\n", dotted);
    return false;
  }
  return true;
}

/* Encodes `packet` once and keeps the bytes, for the decode measurement of the same packet. */
static bool capture(Bench *bench, const char *packet, uint8_t *out, size_t capacity, size_t *length) {
  if (framedsl_encode_into(bench->frame, packet, out, capacity, length) != FRAMEDSL_OK) {
    fprintf(stderr, "bench: could not encode %s\n", packet);
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  const double minimum = argc > 1 ? atof(argv[1]) : 0.25;

  FramedslResult *result = framedsl_load_string(DEFINITION, "bench.fdsl");
  if (!framedsl_ok(result)) {
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *d = framedsl_diagnostic(result, i);
      fprintf(stderr, "bench.fdsl:%d:%d: %s\n", d->line, d->column, d->message);
    }
    framedsl_result_free(result);
    return 1;
  }

  Bench bench = { .definition = framedsl_definition(result) };
  bench.frame = framedsl_frame_new(bench.definition);

  uint8_t data[256];
  memset(data, 0xA5, sizeof(data));

  bool ok = bench.frame != NULL && resolve(&bench, "TC_SET_LIMIT.limit", &bench.limit)
            && resolve(&bench, "TM_BITS.counter", &bench.counter) && resolve(&bench, "TM_PAYLOAD.len", &bench.len)
            && resolve(&bench, "TM_PAYLOAD.data", &bench.data) && resolve(&bench, "TM_IMU.gyro.z", &bench.gyro_z)
            && resolve(&bench, "TM_IMU.bus", &bench.bus) && resolve(&bench, "TM_IMU.sensor", &bench.sensor);

  /* A 200-byte payload: long enough that copying it is part of what is measured. */
  ok = ok && framedsl_set_u64(bench.frame, bench.len, 200) == FRAMEDSL_OK
       && framedsl_set_bytes(bench.frame, bench.data, data, 200) == FRAMEDSL_OK
       && framedsl_set_u64(bench.frame, bench.bus, 2048) == FRAMEDSL_OK
       && framedsl_set_u64(bench.frame, bench.sensor, 1234) == FRAMEDSL_OK;

  ok = ok && capture(&bench, "TC_SET_LIMIT", bench.set_limit, sizeof(bench.set_limit), &bench.set_limit_length)
       && capture(&bench, "TM_BITS", bench.bits, sizeof(bench.bits), &bench.bits_length)
       && capture(&bench, "TM_PAYLOAD", bench.payload, sizeof(bench.payload), &bench.payload_length)
       && capture(&bench, "TM_IMU", bench.imu, sizeof(bench.imu), &bench.imu_length);

  if (!ok) {
    framedsl_frame_free(bench.frame);
    framedsl_result_free(result);
    return 1;
  }

  printf("frame-dsl benchmark: at least %.2f s per measurement, one frame per operation\n\n", minimum);
  measure(&bench, "encode   whole bytes    (8 B)", encode_set_limit, minimum);
  measure(&bench, "decode   whole bytes    (8 B)", decode_set_limit, minimum);
  measure(&bench, "encode   sub-byte, LE   (9 B)", encode_bits, minimum);
  measure(&bench, "decode   sub-byte, LE   (9 B)", decode_bits, minimum);
  measure(&bench, "encode   valof payload  (203 B)", encode_payload, minimum);
  measure(&bench, "decode   valof payload  (203 B)", decode_payload, minimum);
  measure(&bench, "encode   composed       (19 B)", encode_imu, minimum);
  measure(&bench, "decode   composed       (19 B)", decode_imu, minimum);
  measure(&bench, "get_eng  linear curve", read_linear, minimum);
  measure(&bench, "get_eng  bytecode curve", read_curve, minimum);
  measure(&bench, "set_eng  linear curve", write_linear, minimum);
  measure(&bench, "set_eng  explicit inverse", write_curve, minimum);

  framedsl_frame_free(bench.frame);
  framedsl_result_free(result);
  return 0;
}
