/* Where memory sits, and what decoding costs (#109).
 *
 * The first baseline saw one decode move by up to 30% from one process to the next, and not at all with
 * address randomisation off. This puts that to the test in one process, so a change can be checked
 * against it without twenty runs: it decodes one frame, timing the best of several batches, with the
 * stack -- or, separately, the frame's bytes -- shifted 64 bytes at a time across a whole 4 KiB page.
 * A decode that stores to the stack while it reads the heap is the kind this finds: on main before the
 * performance round, shifting the stack moved `size/fixed/256` decode by 13%, shifting the buffer by 5%.
 *
 *   build/layout [stack|buffer] [bytes]
 *
 * Prints one line per shift, `shift ns`, then the spread. Not a test: nothing fails on a wide one. */
#define _GNU_SOURCE
#include <alloca.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "framedsl.h"

static volatile size_t sink;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* The best of seven batches: a shift that makes decoding slower does so every time. */
static double timed(FramedslFrame *frame, const uint8_t *wire, size_t length) {
  double best = 1e30;
  for (int batch = 0; batch < 7; ++batch) {
    double start = now();
    for (int i = 0; i < 20000; ++i) {
      size_t consumed = 0;
      framedsl_decode(frame, wire, length, &consumed, NULL);
      sink += consumed;
    }
    double ns = (now() - start) * 1e9 / 20000;
    best = ns < best ? ns : best;
  }
  return best;
}

/* The same, with `shift` more bytes of stack in use: everything the decode puts there moves with it. */
__attribute__((noinline)) static double shifted(
  size_t shift,
  FramedslFrame *frame,
  const uint8_t *wire,
  size_t length
) {
  volatile char *pad = alloca(shift + 1);
  pad[0] = 0;
  return timed(frame, wire, length);
}

int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "stack";
  const int bytes = argc > 2 ? atoi(argv[2]) : 256;
  if ((strcmp(what, "stack") != 0 && strcmp(what, "buffer") != 0) || bytes < 4 || bytes > 4096) {
    fputs("usage: layout [stack|buffer] [bytes, 4 to 4096]\n", stderr);
    return 2;
  }

  /* One CPU, as the baseline does: a migration mid-batch is noise, not layout. */
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof(allowed), &allowed) == 0) {
    for (int c = CPU_SETSIZE - 1; c >= 0; --c) {
      if (CPU_ISSET(c, &allowed)) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(c, &one);
        sched_setaffinity(0, sizeof(one), &one);
        break;
      }
    }
  }

  /* The baseline's `size/fixed` packet: an id, a sequence number and a bit string. */
  char source[128];
  snprintf(source, sizeof(source), "pkt P { id: 8 := 0x5A; seq: 16; data: %d; };\n", (bytes - 3) * 8);
  FramedslResult *result = framedsl_load_string(source, "layout.fdsl");
  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = definition != NULL ? framedsl_frame_new(definition) : NULL;
  uint8_t *pool = aligned_alloc(4096, 3 * 4096);
  if (frame == NULL || pool == NULL) {
    fputs("layout: could not set up\n", stderr);
    return 1;
  }
  memset(pool, 0xA5, 3 * 4096);

  double fastest = 1e30;
  double slowest = 0;
  for (size_t shift = 0; shift < 4096; shift += 64) {
    uint8_t *wire = strcmp(what, "buffer") == 0 ? pool + shift : pool;
    wire[0] = 0x5A;
    double ns =
      strcmp(what, "stack") == 0 ? shifted(shift, frame, wire, (size_t)bytes) : timed(frame, wire, (size_t)bytes);
    printf("%4zu %8.1f\n", shift, ns);
    fastest = ns < fastest ? ns : fastest;
    slowest = ns > slowest ? ns : slowest;
  }
  printf(
    "# %s shifted across a page: %.1f to %.1f ns, %.0f%% apart\n",
    what,
    fastest,
    slowest,
    (slowest - fastest) / fastest * 100
  );

  free(pool);
  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return 0;
}
