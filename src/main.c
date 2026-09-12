/* frame-dsl: validate a definition file, describe what is in it, and decode bytes against it.
 *
 * Written against the public header alone, with nothing internal included. That is deliberate: if
 * the tool shipped with the project needs to reach inside, the interface is not finished. Decoding a
 * capture turned out to need nothing that was not already public, which is the useful half of that
 * discipline -- it is an answer, not a rule. */
#include <stdio.h>
#include <string.h>

#include "framedsl.h"
#include "logger.h"

/* The largest capture this tool will take, in bytes. A frame is a few dozen bytes and a hex string
 * on a command line is bounded by the shell long before this; the number exists so that the buffer
 * is a fixed size rather than something sized from the input. */
#define MAX_CAPTURE 65536

static void report(const FramedslResult *result) {
  const char *name = framedsl_source_name(result);

  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);

    /* The check number is printed because it is what makes a rejection checkable: "rejected" is a
     * weaker claim than "rejected for the reason the file says it should be". */
    char check[32] = "";
    if (diag->check > 0) {
      snprintf(check, sizeof(check), " [check %d]", diag->check);
    }

    if (diag->line > 0) {
      log_fail("%s:%d:%d:%s %s", name, diag->line, diag->column, check, diag->message);
    } else {
      log_fail("%s:%s %s", name, check, diag->message);
    }
  }
}

static void describe(const FramedslDefinition *definition) {
  for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
    size_t fields = framedsl_field_count(definition, p);

    log_info("pkt %s: %zu field%s, %llu bits reserved", framedsl_packet_name(definition, p), fields,
             fields == 1 ? "" : "s",
             (unsigned long long)framedsl_packet_reserved_bits(definition, p));

    for (size_t f = 0; f < fields; ++f) {
      FramedslField field;
      if (!framedsl_field(definition, p, f, &field)) {
        continue;
      }

      char offset[32] = "at ?";
      if (field.offset_known) {
        snprintf(offset, sizeof(offset), "at %llu", (unsigned long long)field.offset_bits);
      }

      if (field.variable) {
        /* Reported in the unit it was written in, so the line can be compared with the file. */
        log_info("    %-14s %s, variable, reserves %llu bytes", field.name, offset,
                 (unsigned long long)(field.max_bits / 8));
      } else if (field.has_fixed) {
        log_info("    %-14s %s, %llu bits = %#llx", field.name, offset,
                 (unsigned long long)field.width_bits, (unsigned long long)field.fixed);
      } else {
        log_info("    %-14s %s, %llu bits", field.name, offset,
                 (unsigned long long)field.width_bits);
      }
    }
  }
}

/* ---- Hex --------------------------------------------------------------------------------------
 *
 * The hex somebody has was copied out of something that chose its own separators -- a log line, a
 * tcpdump column, a colleague's message -- so spaces, colons, commas, dashes and `0x` prefixes are
 * all tolerated. What is not tolerated is an odd number of digits: that is a typo, and a tool that
 * padded it would silently change the frame it was asked about. */

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static bool is_separator(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ':' || c == '-' ||
         c == '_' || c == '.';
}

/* Bytes into `out`, count into `*length`. False on a problem, having said what it was. */
static bool parse_hex(const char *text, uint8_t *out, size_t capacity, size_t *length) {
  size_t count = 0;
  int high = -1;

  for (const char *at = text; *at != '\0'; ++at) {
    if (is_separator(*at)) {
      /* A separator in the middle of a byte means somebody wrote a byte with one digit -- `0 1 00`
       * rather than `01 00`. Padding it to `01` would be a guess, and the guess changes the frame. */
      if (high >= 0) {
        log_fail("hex: byte %zu has only one digit (at offset %zu)", count, (size_t)(at - text));
        return false;
      }
      continue;
    }

    /* A `0x` prefix, but only where one can start: `0x0x1` is not two prefixes. */
    if (high < 0 && (*at == '0') && (at[1] == 'x' || at[1] == 'X') && hex_value(at[2]) >= 0) {
      at += 1;
      continue;
    }

    int digit = hex_value(*at);
    if (digit < 0) {
      log_fail("hex: '%c' is not a hex digit (at offset %zu)", *at, (size_t)(at - text));
      return false;
    }

    if (high < 0) {
      high = digit;
      continue;
    }
    if (count >= capacity) {
      log_fail("hex: more than %d bytes", MAX_CAPTURE);
      return false;
    }
    out[count++] = (uint8_t)((high << 4) | digit);
    high = -1;
  }

  if (high >= 0) {
    log_fail("hex: an odd number of digits; the last byte is incomplete");
    return false;
  }
  if (count == 0) {
    log_fail("hex: no bytes to decode");
    return false;
  }

  *length = count;
  return true;
}

/* ---- Decoding a capture ------------------------------------------------------------------------ */

/* A field's bytes as hex, into `out`. Long payloads are cut short: this is a line somebody reads, and
 * the whole of a 64KB field is not one. */
static void format_bytes(const uint8_t *bytes, size_t length, char *out, size_t capacity) {
  const size_t shown = length < 24 ? length : 24;
  size_t used = 0;

  for (size_t i = 0; i < shown && used + 4 < capacity; ++i) {
    used += (size_t)snprintf(out + used, capacity - used, i == 0 ? "%02X" : " %02X", bytes[i]);
  }
  if (shown < length && used + 8 < capacity) {
    snprintf(out + used, capacity - used, " ... (+%zu more)", length - shown);
  }
}

/* Prints every field of the packet that was decoded. */
static void report_frame(const FramedslDefinition *definition, const FramedslFrame *frame,
                         const char *name, size_t consumed) {
  size_t packet = 0;
  if (!framedsl_find_packet(definition, name, &packet)) {
    return;
  }

  log_info("%s, %zu byte%s", name, consumed, consumed == 1 ? "" : "s");

  for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
    FramedslField field;
    FramedslFieldId id;
    if (!framedsl_field(definition, packet, f, &field) ||
        !framedsl_field_id(definition, packet, f, &id)) {
      continue;
    }

    if (field.is_scalar) {
      uint64_t value = 0;
      if (framedsl_get_u64(frame, id, &value) == FRAMEDSL_OK) {
        log_info("    %-14s %llu (%#llx)", field.name, (unsigned long long)value,
                 (unsigned long long)value);
      }
      continue;
    }

    /* A bit string, including every valof()-sized field. The length is what arrived, not what the
     * field reserved -- which is the whole point of asking the frame rather than the definition.
     * Static because a field can reserve as much as a capture can hold, and that does not belong on
     * a stack. */
    static uint8_t bytes[MAX_CAPTURE];
    size_t length = 0;
    char text[128] = "";

    if (framedsl_get_bytes(frame, id, bytes, sizeof(bytes), &length) == FRAMEDSL_OK) {
      format_bytes(bytes, length, text, sizeof(text));
      log_info("    %-14s [%zu byte%s] %s", field.name, length, length == 1 ? "" : "s", text);
    }
  }
}

static const char *decode_problem(FramedslStatus status) {
  switch (status) {
  case FRAMEDSL_ERROR_UNRECOGNISED:
    return "no packet in this definition matches these bytes";
  case FRAMEDSL_ERROR_TRUNCATED:
    return "a packet matched and the bytes ran out";
  case FRAMEDSL_ERROR_RANGE:
    return "a length field claims more than its field reserved";
  default:
    return "the frame could not be decoded";
  }
}

/* Walks the capture frame by frame, which is what `consumed` is for: a packet with a valof() field
 * has no length until it has been decoded, so a caller cannot find the next one without this. */
static int decode_capture(const FramedslDefinition *definition, const uint8_t *bytes,
                          size_t length) {
  FramedslFrame *frame = framedsl_frame_new(definition);
  if (frame == NULL) {
    log_fail("out of memory");
    return 2;
  }

  int status = 0;
  size_t offset = 0;
  size_t count = 0;

  while (offset < length) {
    size_t consumed = 0;
    FramedslStatus why = FRAMEDSL_OK;
    const char *name =
        framedsl_decode(frame, bytes + offset, length - offset, &consumed, &why);

    if (name == NULL) {
      /* The offset is in the message because a capture holding several frames fails somewhere, and
       * "somewhere" is the first thing anybody wants to know. */
      log_fail("at byte %zu: %s", offset, decode_problem(why));
      status = 1;
      break;
    }

    report_frame(definition, frame, name, consumed);
    offset += consumed;
    count += 1;
  }

  if (status == 0) {
    log_info("%zu frame%s, %zu byte%s", count, count == 1 ? "" : "s", length,
             length == 1 ? "" : "s");
  }

  framedsl_frame_free(frame);
  return status;
}

static void usage(const char *program) {
  log_fail("usage: %s <definition.fdsl> [-d|--decode <hex>]", program);
  log_fail("  with no -d, describes the definition");
  log_fail("  %s frames.fdsl -d \"AA BB CC DD\"", program);
}

int main(int argc, char **argv) {
  const char *path = NULL;
  const char *hex = NULL;

  /* The flag may come before or after the file. Nothing here needs an argument parser: two shapes,
   * and anything else is a usage error rather than something to guess at. */
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0) {
      if (i + 1 >= argc) {
        log_fail("%s: expected hex bytes after it", argv[i]);
        usage(argv[0]);
        return 2;
      }
      hex = argv[++i];
    } else if (path == NULL) {
      path = argv[i];
    } else {
      log_fail("unexpected argument '%s'", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  if (path == NULL) {
    usage(argv[0]);
    return 2;
  }

  FramedslResult *result = framedsl_load_file(path);
  if (result == NULL) {
    log_fail("out of memory");
    return 2;
  }

  int status;
  switch (framedsl_status(result)) {
  case FRAMEDSL_OK: {
    const FramedslDefinition *definition = framedsl_definition(result);

    if (hex != NULL) {
      /* Static rather than automatic: 64KB is more than a stack frame should carry, and the whole
       * point of the number is that it is fixed. */
      static uint8_t capture[MAX_CAPTURE];
      size_t length = 0;

      if (!parse_hex(hex, capture, sizeof(capture), &length)) {
        status = 2;
        break;
      }
      status = decode_capture(definition, capture, length);
      break;
    }

    describe(definition);
    log_info("%s: ok, %zu packet%s", framedsl_source_name(result),
             framedsl_packet_count(definition), framedsl_packet_count(definition) == 1 ? "" : "s");
    status = 0;
    break;
  }
  case FRAMEDSL_ERROR_INVALID:
    report(result);
    status = 1;
    break;
  /* A missing file is not a malformed file, and a script should not have to grep the message to
   * tell them apart. */
  case FRAMEDSL_ERROR_IO:
  case FRAMEDSL_ERROR_MEMORY:
  default:
    report(result);
    status = 2;
    break;
  }

  framedsl_result_free(result);
  return status;
}
