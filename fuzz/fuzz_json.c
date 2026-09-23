/* Fuzz target: arbitrary bytes as a JSON-RPC message (#30).
 *
 * The one part of the language server an editor -- or anything else that can reach its standard input
 * -- hands unchecked bytes to. Whatever comes in, the reader must either refuse it or produce a tree
 * that is what the text said; it must not read out of bounds, loop, or run the stack off the end.
 *
 * Unlike the other targets this one reaches inside the library: json.h is not public, and a JSON
 * reader nobody outside this repository calls has no business being.
 *
 * Every value that parses is written back out and read again: what the writer produces must be JSON
 * the reader accepts, which is the property the protocol actually depends on. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Region *region = region_new();
  if (region == NULL) {
    return 0;
  }

  const Json *value = json_parse(region, (const char *)data, size);
  if (value != NULL) {
    JsonWriter writer;
    json_writer_init(&writer);
    json_write_value(&writer, value);
    if (!writer.failed) {
      Region *again = region_new();
      if (again != NULL && json_parse(again, writer.data, writer.length) == NULL) {
        abort(); /* the writer produced something the reader will not take */
      }
      region_free(again);
    }
    json_writer_free(&writer);
  }

  region_free(region);
  return 0;
}
