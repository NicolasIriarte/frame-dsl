/* Fuzz target: arbitrary text through the formatter (#29).
 *
 * Whatever does not parse must be refused without crashing. Whatever does parse makes two promises,
 * and both are checked here: formatting it again changes nothing, and the formatted text means what
 * the original did -- it loads if and only if the original loads, and then to the same packets with
 * the same fields at the same offsets and widths.
 *
 * Inputs mentioning `import` are skipped, for the reason fuzz_load.c gives. Built with libFuzzer by
 * `make fuzz` and replayed over fuzz/corpus/format/ by `make test`. Against the public header alone. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"

static char *terminated(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (text == NULL) {
    return NULL;
  }
  memcpy(text, data, size);
  text[size] = '\0';
  return text;
}

/* Every field of every packet, compared by what the layout says about it rather than by bytes: the
 * tag that tells two definitions' ids apart differs between any two loads. */
static void same_definition(const FramedslDefinition *a, const FramedslDefinition *b) {
  if (framedsl_packet_count(a) != framedsl_packet_count(b) || framedsl_cal_count(a) != framedsl_cal_count(b)) {
    abort();
  }
  for (size_t p = 0; p < framedsl_packet_count(a); ++p) {
    if (strcmp(framedsl_packet_name(a, p), framedsl_packet_name(b, p)) != 0
        || framedsl_field_count(a, p) != framedsl_field_count(b, p)) {
      abort();
    }
    for (size_t f = 0; f < framedsl_field_count(a, p); ++f) {
      FramedslField x;
      FramedslField y;
      if (!framedsl_field(a, p, f, &x) || !framedsl_field(b, p, f, &y)) {
        abort();
      }
      bool same_cal = (x.cal == NULL) == (y.cal == NULL) && (x.cal == NULL || strcmp(x.cal, y.cal) == 0);
      if (strcmp(x.name, y.name) != 0 || x.variable != y.variable || x.width_bits != y.width_bits
          || x.max_bits != y.max_bits || x.offset_known != y.offset_known || x.offset_bits != y.offset_bits
          || x.has_fixed != y.has_fixed || x.fixed != y.fixed || x.bit_order != y.bit_order
          || x.byte_order != y.byte_order || !same_cal) {
        abort();
      }
    }
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *source = terminated(data, size);
  if (source == NULL || strstr(source, "import") != NULL) {
    free(source);
    return 0;
  }

  char *once = NULL;
  FramedslStatus status = framedsl_format(source, &once);
  if (status != FRAMEDSL_OK) {
    if (status != FRAMEDSL_ERROR_INVALID) {
      abort();
    }
    free(source);
    return 0;
  }

  char *twice = NULL;
  if (framedsl_format(once, &twice) != FRAMEDSL_OK || strcmp(once, twice) != 0) {
    abort();
  }

  FramedslResult *before = framedsl_load_string(source, "before");
  FramedslResult *after = framedsl_load_string(once, "after");
  if (framedsl_ok(before) != framedsl_ok(after)) {
    abort();
  }
  if (framedsl_ok(before)) {
    same_definition(framedsl_definition(before), framedsl_definition(after));
  }

  framedsl_result_free(before);
  framedsl_result_free(after);
  free(twice);
  free(once);
  free(source);
  return 0;
}
