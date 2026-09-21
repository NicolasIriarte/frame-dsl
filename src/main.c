/* frame-dsl: validate a definition file, describe what is in it, and decode or encode bytes against
 * it.
 *
 * Written against the public header alone, with nothing internal included. That is deliberate: if
 * the tool shipped with the project needs to reach inside, the interface is not finished. Decoding a
 * capture turned out to need nothing that was not already public, which is the useful half of that
 * discipline -- it is an answer, not a rule. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

    log_info(
      "pkt %s: %zu field%s, %llu bits reserved",
      framedsl_packet_name(definition, p),
      fields,
      fields == 1 ? "" : "s",
      (unsigned long long)framedsl_packet_reserved_bits(definition, p)
    );

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
        log_info(
          "    %-14s %s, variable, reserves %llu bytes", field.name, offset, (unsigned long long)(field.max_bits / 8)
        );
      } else if (field.has_fixed) {
        log_info(
          "    %-14s %s, %llu bits = %#llx",
          field.name,
          offset,
          (unsigned long long)field.width_bits,
          (unsigned long long)field.fixed
        );
      } else {
        log_info("    %-14s %s, %llu bits", field.name, offset, (unsigned long long)field.width_bits);
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
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ':' || c == '-' || c == '_' || c == '.';
}

/* Bytes into `out`, count into `*length`. False on a problem, having said what it was. `allow_empty`
 * is for a field value, where no bytes is a legitimate answer; a capture of nothing is not. */
static bool parse_hex(const char *text, uint8_t *out, size_t capacity, size_t *length, bool allow_empty) {
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
  if (count == 0 && !allow_empty) {
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

/* Prints every field of the packet that was decoded, to `out`: stdout when decoding, where the report
 * is the answer, and stderr when encoding, where the answer is the hex and the report must not end up
 * in whatever it is piped into. */
static void report_frame(
  FILE *out,
  const FramedslDefinition *definition,
  const FramedslFrame *frame,
  const char *name,
  size_t consumed
) {
  size_t packet = 0;
  if (!framedsl_find_packet(definition, name, &packet)) {
    return;
  }

  fprintf(out, "# Decoded %zu byte%s\n", consumed, consumed == 1 ? "" : "s");
  fprintf(out, "pkt %s {\n", name);

  for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
    FramedslField field;
    FramedslFieldId id;
    if (!framedsl_field(definition, packet, f, &field) || !framedsl_field_id(definition, packet, f, &id)) {
      continue;
    }

    if (field.is_scalar) {
      uint64_t value = 0;
      if (framedsl_get_u64(frame, id, &value) == FRAMEDSL_OK) {
        fprintf(out, "    %-14s %llu (%#llx)\n", field.name, (unsigned long long)value, (unsigned long long)value);
      }
      continue;
    }

    /* A bit string, including every valof()-sized field. The length is what arrived, not what the
     * field reserved -- which is the whole point of asking the frame rather than the definition.
     * Nothing is copied: this tool only prints the bytes, and they are already in the frame. */
    size_t length = 0;
    char text[128] = "";

    const uint8_t *bytes = framedsl_get_bytes(frame, id, &length, NULL);
    if (bytes != NULL) {
      format_bytes(bytes, length, text, sizeof(text));
      fprintf(out, "    %-14s [%zu byte%s] %s\n", field.name, length, length == 1 ? "" : "s", text);
    }
  }
  fprintf(out, "}\n");
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
static int decode_capture(const FramedslDefinition *definition, const uint8_t *bytes, size_t length) {
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
    const char *name = framedsl_decode(frame, bytes + offset, length - offset, &consumed, &why);

    if (name == NULL) {
      /* The offset is in the message because a capture holding several frames fails somewhere, and
       * "somewhere" is the first thing anybody wants to know. */
      log_fail("at byte %zu: %s", offset, decode_problem(why));
      status = 1;
      break;
    }

    report_frame(stdout, definition, frame, name, consumed);
    offset += consumed;
    count += 1;
  }

  if (status == 0) {
    log_base("%zu frame%s, %zu byte%s", count, count == 1 ? "" : "s", length, length == 1 ? "" : "s");
  }

  framedsl_frame_free(frame);
  return status;
}

/* ---- Encoding a frame ---------------------------------------------------------------------------
 *
 * The other direction: somebody knows the packet they want and needs its bytes -- to paste into a
 * simulator, to push onto a bus by hand, or to send a deliberately odd value and see what the other
 * end does with it. Fields not named encode as zero, which is the runtime's rule and not one this
 * tool adds. */

/* A scalar's value: decimal, or hex with a `0x` prefix. A leading zero is not octal -- nobody typing
 * a sequence number means 010 to be eight. */
static bool parse_number(const char *text, uint64_t *out) {
  const bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  const char *digits = hex ? text + 2 : text;

  /* strtoull() takes a sign and whitespace and would turn "-1" into the largest value there is. */
  if (*digits == '\0' || *digits == '-' || *digits == '+' || *digits == ' ') {
    return false;
  }

  char *end = NULL;
  errno = 0;
  unsigned long long value = strtoull(digits, &end, hex ? 16 : 10);
  if (errno != 0 || *end != '\0') {
    return false;
  }
  *out = (uint64_t)value;
  return true;
}

/* One `field=value` onto the frame. Returns an exit status: 0, 1 when the value does not fit the
 * definition, 2 when the argument itself is malformed -- the same line decoding draws between bytes
 * that are wrong and hex that is not hex. */
static int assign(
  const FramedslDefinition *definition,
  FramedslFrame *frame,
  size_t packet,
  const char *assignment,
  bool *given
) {
  const char *equals = strchr(assignment, '=');
  const size_t name_length = (size_t)(equals - assignment);
  const char *text = equals + 1;
  const char *packet_name = framedsl_packet_name(definition, packet);

  char name[128];
  if (name_length == 0 || name_length >= sizeof(name)) {
    log_fail("'%s': expected field=value", assignment);
    return 2;
  }
  memcpy(name, assignment, name_length);
  name[name_length] = '\0';

  size_t index = 0;
  FramedslField field;
  FramedslFieldId id;
  if (!framedsl_find_field(definition, packet, name, &index)) {
    log_fail("%s has no field '%s'", packet_name, name);
    return 1;
  }
  if (!framedsl_field(definition, packet, index, &field) || !framedsl_field_id(definition, packet, index, &id)) {
    log_fail("%s.%s could not be resolved", packet_name, name);
    return 1;
  }

  /* The encoder writes a fixed value from the definition whatever the slot holds, so accepting one
   * here would print bytes that silently disagree with what was asked for. */
  if (field.has_fixed) {
    log_fail("%s.%s is fixed at %#llx by the definition", packet_name, name, (unsigned long long)field.fixed);
    return 1;
  }
  if (given[index]) {
    log_fail("%s.%s is given twice", packet_name, name);
    return 2;
  }
  given[index] = true;

  if (field.is_scalar) {
    uint64_t value = 0;
    if (!parse_number(text, &value)) {
      log_fail("%s.%s: '%s' is not a number (decimal, or hex with 0x)", packet_name, name, text);
      return 2;
    }
    if (framedsl_set_u64(frame, id, value) != FRAMEDSL_OK) {
      log_fail(
        "%s.%s: %s is too wide for its %llu bits", packet_name, name, text, (unsigned long long)field.width_bits
      );
      return 1;
    }
    return 0;
  }

  /* A bit string takes hex, written any way -d takes it. Static for the same reason as the capture:
   * the size is fixed, and it is more than a stack frame should carry. */
  static uint8_t bytes[MAX_CAPTURE];
  size_t length = 0;
  if (!parse_hex(text, bytes, sizeof(bytes), &length, true)) {
    log_fail("%s.%s is a bit string and takes hex", packet_name, name);
    return 2;
  }
  if (framedsl_set_bytes(frame, id, bytes, length) != FRAMEDSL_OK) {
    if (field.variable) {
      log_fail(
        "%s.%s reserves %llu bytes and was given %zu",
        packet_name,
        name,
        (unsigned long long)(field.max_bits / 8),
        length
      );
    } else {
      log_fail(
        "%s.%s is exactly %llu bytes and was given %zu",
        packet_name,
        name,
        (unsigned long long)(field.width_bits / 8),
        length
      );
    }
    return 1;
  }
  return 0;
}

/* The one way an encode fails once every value has been accepted: a valof() field whose bytes and
 * length field disagree. The public header does not say which field sizes which, so the message names
 * the variable fields and what was given for each, rather than guessing at the length field. */
static void explain_length_mismatch(
  const FramedslDefinition *definition,
  const FramedslFrame *frame,
  size_t packet,
  const bool *given
) {
  const char *packet_name = framedsl_packet_name(definition, packet);

  for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
    FramedslField field;
    FramedslFieldId id;
    if (!framedsl_field(definition, packet, f, &field) || !field.variable
        || !framedsl_field_id(definition, packet, f, &id)) {
      continue;
    }

    if (given[f]) {
      log_fail(
        "%s.%s was given %zu bytes and the field that sizes it does not say so; set that field as well",
        packet_name,
        field.name,
        framedsl_byte_length(frame, id)
      );
    } else {
      log_fail("%s.%s was not given, so the field that sizes it must be zero", packet_name, field.name);
    }
  }
}

static int encode_packet(
  const FramedslDefinition *definition,
  const char *name,
  const char *const *assignments,
  size_t count,
  bool show,
  bool quiet
) {
  size_t packet = 0;
  if (!framedsl_find_packet(definition, name, &packet)) {
    log_fail("no packet named '%s' in this definition", name);
    return 1;
  }

  FramedslFrame *frame = framedsl_frame_new(definition);
  bool *given = calloc(framedsl_field_count(definition, packet) + 1, sizeof(*given));
  if (frame == NULL || given == NULL) {
    framedsl_frame_free(frame);
    free(given);
    log_fail("out of memory");
    return 2;
  }

  int status = 0;
  for (size_t i = 0; i < count && status == 0; ++i) {
    status = assign(definition, frame, packet, assignments[i], given);
  }

  if (status == 0) {
    size_t length = 0;
    FramedslStatus why = FRAMEDSL_OK;
    const uint8_t *bytes = framedsl_encode(frame, name, &length, &why);

    if (bytes == NULL) {
      if (why == FRAMEDSL_ERROR_RANGE) {
        explain_length_mismatch(definition, frame, packet, given);
      } else {
        log_fail("%s could not be encoded", name);
      }
      status = 1;
    } else {
      /* Spaced hex: it pastes straight back into -d, and `xxd -r -p` reads it as it is. */
      for (size_t i = 0; i < length; ++i) {
        printf(i == 0 ? "%02X" : " %02X", bytes[i]);
      }
      printf("\n");
      fflush(stdout);

      /* Zero is the rule for a field nobody named, and it is a legitimate value -- but a field left
       * out by mistake looks exactly like one left out on purpose, so each is said once. On stderr,
       * like the report, so that what is piped is still only the frame. */
      if (!quiet) {
        for (size_t f = 0; f < framedsl_field_count(definition, packet); ++f) {
          FramedslField field;
          if (!given[f] && framedsl_field(definition, packet, f, &field) && !field.has_fixed) {
            fprintf(stderr, "[INFO] %s.%s not given, encoded as zero\n", name, field.name);
          }
        }
      }

      /* The report is of the bytes, decoded back, rather than of what was typed: it shows the fixed
       * values that went out and proves the frame reads as the packet it was meant to be. */
      if (show) {
        FramedslFrame *check = framedsl_frame_new(definition);
        size_t consumed = 0;
        const char *decoded = check == NULL ? NULL : framedsl_decode(check, bytes, length, &consumed, NULL);
        if (decoded != NULL) {
          report_frame(stderr, definition, check, decoded, consumed);
        }
        framedsl_frame_free(check);
      }
    }
  }

  free(given);
  framedsl_frame_free(frame);
  return status;
}

static void usage(const char *program) {
  log_fail("usage: %s <definition.fdsl> [-d|--decode <hex>]", program);
  log_fail("       %s <definition.fdsl> -e|--encode <packet> [field=value ...] [-r|--report] [-q|--quiet]", program);
  log_fail("  with neither, describes the definition");
  log_fail("  %s frames.fdsl -d \"AA BB CC DD\"", program);
  log_fail("  %s frames.fdsl -e TC_WRITE size=2 data=AABB crc=0x1234", program);
}

int main(int argc, char **argv) {
  const char *path = NULL;
  const char *hex = NULL;
  const char *packet = NULL;
  bool show = false;
  bool quiet = false;

  /* Every `field=value` is one of these, and there cannot be more of them than arguments. */
  const char **assignments = calloc((size_t)argc, sizeof(*assignments));
  size_t assigned = 0;
  if (assignments == NULL) {
    log_fail("out of memory");
    return 2;
  }

  /* Flags may come before or after the file. Nothing here needs an argument parser: an argument with
   * an `=` in it is a field's value, and anything else unexpected is a usage error rather than
   * something to guess at. */
  for (int i = 1; i < argc; ++i) {
    const bool decode_flag = strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0;
    const bool encode_flag = strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--encode") == 0;

    if (decode_flag || encode_flag) {
      if (i + 1 >= argc) {
        log_fail("%s: expected %s after it", argv[i], decode_flag ? "hex bytes" : "a packet name");
        usage(argv[0]);
        free(assignments);
        return 2;
      }
      if (decode_flag) {
        hex = argv[++i];
      } else {
        packet = argv[++i];
      }
    } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--report") == 0) {
      show = true;
    } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
      quiet = true;
    } else if (strchr(argv[i], '=') != NULL) {
      assignments[assigned++] = argv[i];
    } else if (path == NULL) {
      path = argv[i];
    } else {
      log_fail("unexpected argument '%s'", argv[i]);
      usage(argv[0]);
      free(assignments);
      return 2;
    }
  }

  /* Each of these is a guess the tool would otherwise have to make about which one was meant. */
  const char *conflict = NULL;
  if (hex != NULL && packet != NULL) {
    conflict = "-d and -e are two different questions; ask one at a time";
  } else if (packet == NULL && assigned > 0) {
    conflict = "field=value only means something with -e";
  } else if (packet == NULL && show) {
    conflict = "-r reports the frame -e builds; -d already prints its own";
  } else if (packet == NULL && quiet) {
    conflict = "-q silences what -e says about fields left at zero, and means nothing without it";
  }
  if (conflict != NULL) {
    log_fail("%s", conflict);
    usage(argv[0]);
    free(assignments);
    return 2;
  }

  if (path == NULL) {
    usage(argv[0]);
    free(assignments);
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

      if (!parse_hex(hex, capture, sizeof(capture), &length, false)) {
        status = 2;
        break;
      }
      status = decode_capture(definition, capture, length);
      break;
    }

    if (packet != NULL) {
      status = encode_packet(definition, packet, assignments, assigned, show, quiet);
      break;
    }

    describe(definition);
    log_info(
      "%s: ok, %zu packet%s",
      framedsl_source_name(result),
      framedsl_packet_count(definition),
      framedsl_packet_count(definition) == 1 ? "" : "s"
    );
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
  free(assignments);
  return status;
}
