/* frame-dsl: validate a definition file, describe what is in it, decode or encode bytes against it,
 * and write its calibrations out as C.
 *
 * Written against the public header alone, with nothing internal included. That is deliberate: if
 * the tool shipped with the project needs to reach inside, the interface is not finished. Decoding a
 * capture turned out to need nothing that was not already public, which is the useful half of that
 * discipline -- it is an answer, not a rule.
 *
 * The one exception is `--lsp`, which hands over to the language server (src/lsp.c) and nothing else.
 * That server does reach inside -- semantic highlighting is the lexer's answer, and publishing the
 * lexer would be making an interface for one caller in this repository. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framedsl.h"
#include "logger.h"
#include "lsp.h"

/* The largest capture this tool will take, in bytes. A frame is a few dozen bytes and a hex string
 * on a command line is bounded by the shell long before this; the number exists so that the buffer
 * is a fixed size rather than something sized from the input. */
#define MAX_CAPTURE 65536

static void report(const FramedslResult *result) {
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    /* A problem inside an imported file is reported at its own position, in its own file. */
    const char *name = diag->file != NULL ? diag->file : framedsl_source_name(result);

    /* The check number is printed because it is what makes a rejection checkable: "rejected" is a
     * weaker claim than "rejected for the reason the file says it should be". */
    char check[32] = "";
    if (diag->check > 0) {
      snprintf(check, sizeof(check), " [check %d]", diag->check);
    }

    if (diag->severity == FRAMEDSL_SEVERITY_WARNING) {
      if (diag->line > 0) {
        log_warn("%s:%d:%d:%s %s", name, diag->line, diag->column, check, diag->message);
      } else {
        log_warn("%s:%s %s", name, check, diag->message);
      }
    } else if (diag->line > 0) {
      log_fail("%s:%d:%d:%s %s", name, diag->line, diag->column, check, diag->message);
    } else {
      log_fail("%s:%s %s", name, check, diag->message);
    }
  }
}

/* --check's output: one diagnostic per line in the shape gcc prints, and nothing else -- no colour,
 * no logger prefix, no summary. Every editor already knows how to read this shape (flymake, Vim's
 * errorformat, ALE, a VS Code problem matcher), which is the whole reason to print it. */
static void report_plain(const FramedslResult *result) {
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    const char *name = diag->file != NULL ? diag->file : framedsl_source_name(result);
    const char *severity = diag->severity == FRAMEDSL_SEVERITY_WARNING ? "warning" : "error";

    if (diag->line > 0) {
      printf("%s:%d:%d: %s: %s", name, diag->line, diag->column, severity, diag->message);
    } else {
      printf("%s: %s: %s", name, severity, diag->message);
    }
    if (diag->check > 0) {
      printf(" [check %d]", diag->check);
    }
    putchar('\n');
    /* The fix, where there is one, as a note of its own: the same `file:line:col:` shape, but no
     * editor's pattern for this output takes a note for a problem. */
    if (diag->fix_title != NULL) {
      printf("%s:%d:%d: note: %s\n", name, diag->fix_line, diag->fix_column, diag->fix_title);
    }
  }
}

/* All of standard input, terminated. NULL if it could not be read. */
static char *read_stdin(void) {
  size_t capacity = 4096;
  size_t used = 0;
  char *text = malloc(capacity);
  if (text == NULL) {
    return NULL;
  }
  for (;;) {
    if (used + 1 >= capacity) {
      char *grown = realloc(text, capacity * 2);
      if (grown == NULL) {
        free(text);
        return NULL;
      }
      text = grown;
      capacity *= 2;
    }
    size_t got = fread(text + used, 1, capacity - used - 1, stdin);
    used += got;
    if (got == 0) {
      break;
    }
  }
  if (ferror(stdin)) {
    free(text);
    return NULL;
  }
  text[used] = '\0';
  return text;
}

/* `-` is standard input, attributed to `stdin_name` when there is one: an editor pipes the buffer as
 * typed and says which file it belongs to, so imports resolve from beside it. */
static FramedslResult *load_path(const char *path, const char *stdin_name) {
  if (strcmp(path, "-") != 0) {
    return framedsl_load_file(path);
  }
  char *source = read_stdin();
  FramedslResult *result =
    stdin_name != NULL ? framedsl_load_source(source, stdin_name) : framedsl_load_string(source, "<stdin>");
  free(source);
  return result;
}

/* All of a file, terminated. NULL if it could not be read. */
static char *read_file(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    return NULL;
  }
  size_t capacity = 4096;
  size_t used = 0;
  char *text = malloc(capacity);
  while (text != NULL) {
    if (used + 1 >= capacity) {
      char *grown = realloc(text, capacity * 2);
      if (grown == NULL) {
        free(text);
        text = NULL;
        break;
      }
      text = grown;
      capacity *= 2;
    }
    size_t got = fread(text + used, 1, capacity - used - 1, handle);
    used += got;
    if (got == 0) {
      text[used] = '\0';
      break;
    }
  }
  bool failed = ferror(handle) != 0;
  fclose(handle);
  if (failed) {
    free(text);
    return NULL;
  }
  return text;
}

/* --format on one file. In place, and only when something changes, so a formatted file keeps its
 * timestamp; `-` goes from standard input to standard output. With `check`, nothing is written and a
 * file that would change is named instead. A file that does not parse is left exactly as it is, and
 * its errors are printed the way --check prints them. Returns the file's exit status. */
static int format_one(const char *path, const char *stdin_name, bool check) {
  const bool piped = strcmp(path, "-") == 0;
  const char *name = piped ? (stdin_name != NULL ? stdin_name : "<stdin>") : path;
  char *source = piped ? read_stdin() : read_file(path);
  if (source == NULL) {
    printf("%s: error: cannot read '%s'\n", name, name);
    return 2;
  }

  char *formatted = NULL;
  FramedslStatus status = framedsl_format(source, &formatted);
  if (status == FRAMEDSL_ERROR_INVALID) {
    /* The load stops at the same syntax errors the formatter did, and says what they are. */
    FramedslResult *result = framedsl_load_source(source, name);
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      if (framedsl_diagnostic(result, i)->severity != FRAMEDSL_SEVERITY_ERROR) {
        continue;
      }
      const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
      printf("%s:%d:%d: error: %s\n", diag->file != NULL ? diag->file : name, diag->line, diag->column, diag->message);
    }
    framedsl_result_free(result);
    free(source);
    return 1;
  }
  if (status != FRAMEDSL_OK) {
    printf("%s: error: out of memory\n", name);
    free(source);
    return 2;
  }

  int result = 0;
  const bool changed = strcmp(source, formatted) != 0;
  if (check) {
    if (changed) {
      printf("%s\n", name);
      result = 1;
    }
  } else if (piped) {
    fputs(formatted, stdout);
  } else if (changed) {
    FILE *handle = fopen(path, "wb");
    if (handle == NULL || fputs(formatted, handle) == EOF) {
      printf("%s: error: cannot write '%s'\n", path, path);
      result = 2;
    }
    if (handle != NULL && fclose(handle) != 0) {
      result = 2;
    }
  }
  free(formatted);
  free(source);
  return result;
}

static bool has_warnings(const FramedslResult *result) {
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    if (framedsl_diagnostic(result, i)->severity == FRAMEDSL_SEVERITY_WARNING) {
      return true;
    }
  }
  return false;
}

/* --check: every file, every diagnostic, and an exit status a script can branch on. The worst file
 * decides it: 2 if one could not be read at all, 1 if one is wrong (or only warned, under --werror),
 * 0 otherwise. */
static int check_files(const char **paths, size_t count, const char *stdin_name, bool werror) {
  int status = 0;
  for (size_t i = 0; i < count; ++i) {
    FramedslResult *result = load_path(paths[i], stdin_name);
    if (result == NULL) {
      printf("%s: error: out of memory\n", paths[i]);
      return 2;
    }
    report_plain(result);

    int this_one = 0;
    switch (framedsl_status(result)) {
    case FRAMEDSL_OK:
      this_one = werror && has_warnings(result) ? 1 : 0;
      break;
    case FRAMEDSL_ERROR_INVALID:
      this_one = 1;
      break;
    default:
      this_one = 2;
      break;
    }
    if (this_one > status) {
      status = this_one;
    }
    framedsl_result_free(result);
  }
  return status;
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
        log_info(
          "    %-14s %s, %llu bits%s%s",
          field.name,
          offset,
          (unsigned long long)field.width_bits,
          field.cal != NULL ? ", cal " : "",
          field.cal != NULL ? field.cal : ""
        );
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
      double eng = 0;
      if (framedsl_get_u64(frame, id, &value) != FRAMEDSL_OK) {
        continue;
      }
      /* A calibrated field shows both: the raw value is what is on the wire, and the engineering
       * value is what anybody reading this actually wants to know. */
      if (field.cal != NULL && framedsl_get_eng(frame, id, &eng) == FRAMEDSL_OK) {
        fprintf(
          out,
          "    %-14s %llu (%#llx) = %.10g [%s]\n",
          field.name,
          (unsigned long long)value,
          (unsigned long long)value,
          eng,
          field.cal
        );
      } else {
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

/* The calibrations as a C header, on stdout, for a program that wants them compiled in. */
static int emit_c(const FramedslDefinition *definition) {
  size_t length = framedsl_emit_c(definition, NULL, NULL, 0);
  char *header = malloc(length + 1);
  if (length == 0 || header == NULL) {
    free(header);
    log_fail("the calibrations could not be written as C");
    return 2;
  }
  framedsl_emit_c(definition, NULL, header, length + 1);
  fputs(header, stdout);
  free(header);
  return 0;
}

/* ---- The command line ------------------------------------------------------------------------ */

/* `git describe` of the checkout this was built from, passed in by the Makefile. */
#ifndef FRAMEDSL_VERSION
  #define FRAMEDSL_VERSION "unknown"
#endif

typedef enum {
  OPTION_DECODE,
  OPTION_ENCODE,
  OPTION_REPORT,
  OPTION_QUIET,
  OPTION_EMIT_C,
  OPTION_CHECK,
  OPTION_WERROR,
  OPTION_FORMAT,
  OPTION_STDIN_NAME,
  OPTION_LSP,
  OPTION_HELP,
  OPTION_VERSION,
} OptionId;

typedef struct {
  OptionId id;
  const char *short_name; /* NULL when there is none */
  const char *long_name;
  const char *argument; /* what the next argument is, or NULL when the flag takes none */
  const char *help;
} Option;

/* Every flag there is, in the order --help lists them. The parser looks flags up here and --help
 * prints this, so a flag cannot be accepted without also being documented -- and a script (the Bash
 * completion, a test) can read the flags back out of --help and trust the list is whole. */
static const Option OPTIONS[] = {
  { OPTION_DECODE,     "-d", "--decode",     "<hex>",    "decode bytes into packets and their fields"         },
  { OPTION_ENCODE,     "-e", "--encode",     "<packet>", "encode a packet from field=value, print it as hex"  },
  { OPTION_REPORT,     "-r", "--report",     NULL,       "with -e: also print the frame decoded back"         },
  { OPTION_QUIET,      "-q", "--quiet",      NULL,       "with -e: say nothing of fields left out as zero"    },
  { OPTION_EMIT_C,     NULL, "--emit-c",     NULL,       "write the calibrations to stdout as C"              },
  { OPTION_CHECK,      NULL, "--check",      NULL,       "only report problems; with --format, write nothing" },
  { OPTION_WERROR,     NULL, "--werror",     NULL,       "with --check: fail on a warning too"                },
  { OPTION_FORMAT,     NULL, "--format",     NULL,       "rewrite files in place, in the standard layout"     },
  { OPTION_STDIN_NAME, NULL, "--stdin-name", "<path>",   "the file '-' stands for, for messages and imports"  },
  { OPTION_LSP,        NULL, "--lsp",        NULL,       "be a language server on stdin and stdout"           },
  { OPTION_HELP,       "-h", "--help",       NULL,       "print this help and exit"                           },
  { OPTION_VERSION,    NULL, "--version",    NULL,       "print the version and exit"                         },
};

#define OPTION_COUNT (sizeof(OPTIONS) / sizeof(*OPTIONS))

static const Option *find_option(const char *argument) {
  for (size_t i = 0; i < OPTION_COUNT; ++i) {
    const Option *option = &OPTIONS[i];
    if ((option->short_name != NULL && strcmp(argument, option->short_name) == 0)
        || strcmp(argument, option->long_name) == 0) {
      return option;
    }
  }
  return NULL;
}

/* Plain text on stdout, not the logger: this is the answer to a question, not a problem, and it has
 * to read the same piped into a pager, a file or a completion script. */
static void help(void) {
  fputs(
    "frame-dsl: check, describe, format, decode and encode against a Frame-DSL definition.\n"
    "\n"
    "usage:\n"
    "  frame-dsl <file.fdsl>                                describe its packets and fields\n"
    "  frame-dsl <file.fdsl> -d <hex>                       decode bytes\n"
    "  frame-dsl <file.fdsl> -e <packet> [field=value ...]  encode a packet [-r] [-q]\n"
    "  frame-dsl <file.fdsl> --emit-c                       write the calibrations out as C\n"
    "  frame-dsl <file.fdsl>... --check [--werror]          report problems as path:line:col: message\n"
    "  frame-dsl <file.fdsl>... --format [--check]          rewrite in the standard layout, or only\n"
    "                                                       list the files that would change\n"
    "  frame-dsl --lsp                                      be a language server for an editor\n"
    "\n"
    "options:\n",
    stdout
  );

  for (size_t i = 0; i < OPTION_COUNT; ++i) {
    const Option *option = &OPTIONS[i];
    char flags[64];
    snprintf(
      flags,
      sizeof(flags),
      "%s%s%s%s%s",
      option->short_name != NULL ? option->short_name : "",
      option->short_name != NULL ? ", " : "    ",
      option->long_name,
      option->argument != NULL ? " " : "",
      option->argument != NULL ? option->argument : ""
    );
    printf("  %-24s  %s\n", flags, option->help);
  }

  fputs(
    "\n"
    "arguments:\n"
    "  <file.fdsl>   a definition file; '-' reads it from standard input\n"
    "  <hex>         bytes as hex, one argument, separated any way or not at all: \"AA BB\", AA:BB, AABB\n"
    "  field=value   a field of the -e packet: decimal or 0x hex for a number, hex bytes for a bit\n"
    "                string; a field not given encodes as zero, and a fixed one cannot be given\n"
    "\n"
    "exit status:\n"
    "  0  done, and nothing was wrong\n"
    "  1  the definition, the bytes or a value is wrong; --check found an error (or, with\n"
    "     --werror, a warning); --format --check found a file that would change\n"
    "  2  the command line is wrong, a file cannot be read, or the input is not what it claims to be\n"
    "\n"
    "examples:\n"
    "  frame-dsl examples/uart_device.fdsl\n"
    "  frame-dsl examples/uart_device.fdsl -d \"81 00 07 01\"\n"
    "  frame-dsl examples/uart_device.fdsl -e TC_WRITE size=2 data=AABB crc=0x1234 -r\n"
    "  frame-dsl examples/*.fdsl --check --werror\n"
    "  frame-dsl examples/*.fdsl --format --check\n"
    "  frame-dsl - --check --stdin-name src/device.fdsl < unsaved-buffer\n",
    stdout
  );
}

/* A mistake on the command line gets one line saying what it was, and a pointer to the rest --
 * not the whole help text, which buries the one line that matters. Plain and on stderr, the way
 * every other command-line tool says it, rather than through the logger, which writes to stdout. */
static int usage_error(const char *program, const char *reason) {
  fprintf(stderr, "%s: %s\n", program, reason);
  fprintf(stderr, "try '%s --help'\n", program);
  return 2;
}

int main(int argc, char **argv) {
  const char *path = NULL;
  bool check = false;
  bool format = false;
  bool lsp = false;
  bool werror = false;
  const char *stdin_name = NULL;
  const char *hex = NULL;
  const char *packet = NULL;
  bool show = false;
  bool quiet = false;
  bool emit = false;

  /* Before anything else, and wherever they appear: somebody adding --help to the end of a command
   * that went wrong wants the help, not a complaint about the rest of the command. */
  bool want_version = false;
  for (int i = 1; i < argc; ++i) {
    const Option *option = find_option(argv[i]);
    if (option != NULL && option->id == OPTION_HELP) {
      help();
      return 0;
    }
    want_version |= option != NULL && option->id == OPTION_VERSION;
  }
  if (want_version) {
    printf("frame-dsl %s\n", FRAMEDSL_VERSION);
    return 0;
  }

  /* Every `field=value` is one of these, and there cannot be more of them than arguments. */
  const char **assignments = calloc((size_t)argc, sizeof(*assignments));
  size_t assigned = 0;
  /* Likewise every definition file named; more than one only means something to --check. */
  const char **paths = calloc((size_t)argc, sizeof(*paths));
  size_t path_count = 0;
  if (assignments == NULL || paths == NULL) {
    free(assignments);
    free(paths);
    log_fail("out of memory");
    return 2;
  }

  /* Flags may come before or after the file. Nothing here needs an argument parser: an argument with
   * an `=` in it is a field's value, and anything else unexpected is a usage error rather than
   * something to guess at. */
  for (int i = 1; i < argc; ++i) {
    const Option *option = find_option(argv[i]);

    if (option == NULL) {
      if (strchr(argv[i], '=') != NULL) {
        assignments[assigned++] = argv[i];
      } else if (argv[i][0] != '-' || strcmp(argv[i], "-") == 0) {
        paths[path_count++] = argv[i];
      } else {
        char reason[256];
        snprintf(reason, sizeof(reason), "unexpected argument '%s'", argv[i]);
        free(assignments);
        free(paths);
        return usage_error(argv[0], reason);
      }
      continue;
    }

    const char *value = NULL;
    if (option->argument != NULL) {
      if (i + 1 >= argc) {
        char reason[256];
        snprintf(reason, sizeof(reason), "%s: expected %s after it", argv[i], option->argument);
        free(assignments);
        free(paths);
        return usage_error(argv[0], reason);
      }
      value = argv[++i];
    }

    switch (option->id) {
    case OPTION_DECODE:
      hex = value;
      break;
    case OPTION_ENCODE:
      packet = value;
      break;
    case OPTION_REPORT:
      show = true;
      break;
    case OPTION_QUIET:
      quiet = true;
      break;
    case OPTION_EMIT_C:
      emit = true;
      break;
    case OPTION_CHECK:
      check = true;
      break;
    case OPTION_WERROR:
      werror = true;
      break;
    case OPTION_FORMAT:
      format = true;
      break;
    case OPTION_STDIN_NAME:
      stdin_name = value;
      break;
    case OPTION_LSP:
      lsp = true;
      break;
    case OPTION_HELP:
    case OPTION_VERSION:
      /* Answered before this loop. */
      break;
    }
  }
  path = path_count > 0 ? paths[0] : NULL;

  size_t from_stdin = 0;
  for (size_t i = 0; i < path_count; ++i) {
    from_stdin += strcmp(paths[i], "-") == 0;
  }

  /* Each of these is a guess the tool would otherwise have to make about which one was meant. */
  const char *conflict = NULL;
  if (lsp && (path_count > 0 || check || format || emit || hex != NULL || packet != NULL || werror)) {
    conflict = "--lsp talks to an editor on stdin and stdout, and takes nothing else";
  } else if (hex != NULL && packet != NULL) {
    conflict = "-d and -e are two different questions; ask one at a time";
  } else if (packet == NULL && assigned > 0) {
    conflict = "field=value only means something with -e";
  } else if (packet == NULL && show) {
    conflict = "-r reports the frame -e builds; -d already prints its own";
  } else if (emit && (hex != NULL || packet != NULL)) {
    conflict = "--emit-c writes the calibrations out; it does not decode or encode as well";
  } else if (packet == NULL && quiet) {
    conflict = "-q silences what -e says about fields left at zero, and means nothing without it";
  } else if (format && (emit || hex != NULL || packet != NULL)) {
    conflict = "--format rewrites the definition; it does not decode, encode or emit as well";
  } else if (format && werror) {
    conflict = "--werror is about warnings, and --format does not report any";
  } else if (check && (emit || hex != NULL || packet != NULL)) {
    conflict = "--check only reports on the definition; it does not decode, encode or emit as well";
  } else if (werror && !check) {
    conflict = "--werror decides --check's exit status, and means nothing without it";
  } else if (path_count > 1 && !check && !format) {
    conflict = "only --check and --format take more than one definition file";
  } else if (from_stdin > 1) {
    conflict = "standard input can be read once; name '-' once";
  } else if (stdin_name != NULL && from_stdin == 0) {
    conflict = "--stdin-name names what '-' reads, and means nothing without it";
  }
  if (conflict != NULL) {
    free(assignments);
    free(paths);
    return usage_error(argv[0], conflict);
  }

  /* Before the check below: the server is told what to work on by the editor, not by an argument. */
  if (lsp) {
    free(assignments);
    free(paths);
    return lsp_serve(stdin, stdout);
  }

  if (path == NULL) {
    free(assignments);
    free(paths);
    return usage_error(argv[0], "no definition file; usage: frame-dsl <file.fdsl> [options], or frame-dsl --lsp");
  }

  if (format) {
    int status = 0;
    for (size_t i = 0; i < path_count; ++i) {
      int one = format_one(paths[i], stdin_name, check);
      status = one > status ? one : status;
    }
    free(assignments);
    free(paths);
    return status;
  }

  if (check) {
    int status = check_files(paths, path_count, stdin_name, werror);
    free(assignments);
    free(paths);
    return status;
  }

  FramedslResult *result = load_path(path, stdin_name);
  free(paths);
  if (result == NULL) {
    log_fail("out of memory");
    free(assignments);
    return 2;
  }

  int status;
  switch (framedsl_status(result)) {
  case FRAMEDSL_OK: {
    const FramedslDefinition *definition = framedsl_definition(result);

    /* Not ahead of --emit-c, whose standard output is C and must stay nothing but C. */
    if (!emit) {
      report(result);
    }

    if (emit) {
      status = emit_c(definition);
      break;
    }

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
