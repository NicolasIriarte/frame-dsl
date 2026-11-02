/* Fuzz target: arbitrary text as a definition file.
 *
 * Most inputs are rejected, and that is fine -- the parser, composition and the checks must reject
 * them without crashing, reading out of bounds or leaking. What loads is then used: every packet is
 * encoded with its fields at zero and decoded back, and must come back as the same packet. That is
 * the promise checks 11 and 12 make -- every `auto` packet identifies itself and no two can be confused
 * -- so a definition that loads and breaks it is a checker bug. (All zeros is not always encodable, so
 * a packet the encoder refuses is skipped.) Calibrations are run and written out as
 * C, which walks every compiled curve.
 *
 * Inputs mentioning `import` are skipped: an import reads a file named by the input, and a fuzzer
 * that can name /dev/stdin or a FIFO would hang rather than find anything.
 *
 * Built with libFuzzer by `make fuzz`, and replayed over fuzz/corpus/ and fuzz/regressions/ by a
 * plain driver in `make test` (fuzz/replay.c). Against the public header alone. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"

/* The loader takes a string, so the input is copied and terminated -- and cut at the first NUL, which
 * a file read from disk could not contain either. */
static char *terminated(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (text == NULL) {
    return NULL;
  }
  memcpy(text, data, size);
  text[size] = '\0';
  return text;
}

static void use(const FramedslDefinition *definition) {
  FramedslFrame *frame = framedsl_frame_new(definition);
  FramedslFrame *back = framedsl_frame_new(definition);
  if (frame == NULL || back == NULL) {
    abort();
  }

  for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
    const char *name = framedsl_packet_name(definition, p);
    size_t length = 0;
    FramedslStatus status = FRAMEDSL_OK;
    const uint8_t *bytes = framedsl_encode(frame, name, &length, &status);
    if (bytes == NULL) {
      /* All zeros is not always a frame: a length written as `valof(n) + 1` says one byte while the
       * payload holds none, and the encoder is right to refuse the disagreement. That, and nothing
       * else, is an acceptable refusal here. */
      if (status != FRAMEDSL_ERROR_RANGE) {
        abort();
      }
      continue;
    }

    size_t consumed = 0;

    /* A manual packet identifies nothing, so the program names it, and dispatch never answers with it
     * (#146), however its bytes look. */
    if (framedsl_packet_manual(definition, p)) {
      if (framedsl_decode_as(back, name, bytes, length, &consumed) != FRAMEDSL_OK || consumed != length) {
        abort();
      }
      const char *found = framedsl_decode(back, bytes, length, NULL, NULL);
      size_t index = 0;
      if (found != NULL
          && (!framedsl_find_packet(definition, found, &index) || framedsl_packet_manual(definition, index))) {
        abort();
      }
      continue;
    }

    const char *decoded = framedsl_decode(back, bytes, length, &consumed, NULL);
    if (decoded == NULL || strcmp(decoded, name) != 0 || consumed != length) {
      abort();
    }
  }

  for (size_t c = 0; c < framedsl_cal_count(definition); ++c) {
    FramedslCalId cal;
    double out;
    if (!framedsl_find_cal(definition, framedsl_cal_name(definition, c), &cal)) {
      abort();
    }
    (void)framedsl_calibrate(definition, cal, 1234.0, &out);
    (void)framedsl_uncalibrate(definition, cal, 1.5, &out);
  }

  size_t header = framedsl_emit_c(definition, NULL, NULL, 0);
  char *text = malloc(header + 1);
  if (text != NULL) {
    framedsl_emit_c(definition, NULL, text, header + 1);
    free(text);
  }

  framedsl_frame_free(back);
  framedsl_frame_free(frame);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = terminated(data, size);
  if (text == NULL || strstr(text, "import") != NULL) {
    free(text);
    return 0;
  }

  FramedslResult *result = framedsl_load_string(text, "fuzz.fdsl");
  if (framedsl_ok(result)) {
    use(framedsl_definition(result));
  } else {
    /* The diagnostics are part of the output: walk them, as the tool does. */
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
      if (diag == NULL || diag->message == NULL) {
        abort();
      }
    }
  }

  framedsl_result_free(result);
  free(text);
  return 0;
}
