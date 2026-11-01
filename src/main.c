/* frame-dsl: validate a definition file, describe what is in it, decode bytes against it, and write its
 * calibrations out as C. Encoding left the command line in #97: a script does it better, through the
 * library.
 *
 * Written against the public header alone, with nothing internal included. That is deliberate: if
 * the tool shipped with the project needs to reach inside, the interface is not finished. Decoding a
 * capture turned out to need nothing that was not already public, which is the useful half of that
 * discipline -- it is an answer, not a rule.
 *
 * The one exception is `--lsp`, which hands over to the language server (src/lsp.c) and nothing else.
 * That server does reach inside -- semantic highlighting is the lexer's answer, and publishing the
 * lexer would be making an interface for one caller in this repository. */
/* isatty() is POSIX, not C11. */
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "framedsl.h"
#include "lsp.h"

/* The largest capture this tool will take, in bytes. A frame is a few dozen bytes and a hex string
 * on a command line is bounded by the shell long before this; the number exists so that the buffer
 * is a fixed size rather than something sized from the input. */
#define MAX_CAPTURE 65536

#define COLOUR_RED "\x1b[31m"
#define COLOUR_YELLOW "\x1b[33m"
#define COLOUR_RESET "\x1b[0m"

/* Only a person reads a terminal. An empty NO_COLOR says nothing, as https://no-color.org has it. */
static bool stderr_takes_colour(void) {
  const char *no_colour = getenv("NO_COLOR");
  return isatty(STDERR_FILENO) && (no_colour == NULL || no_colour[0] == '\0');
}

/* A problem with what was asked, as opposed to with a definition: on stderr, where no result ever goes,
 * under the tool's name, as any command-line tool says it. The name is red on a terminal, so a problem
 * stands out from the results around it. Results are flushed first, so that the two arrive in the order
 * they happened wherever they are sent. */
__attribute__((format(printf, 1, 2))) static void fail(const char *format, ...) {
  fflush(stdout);
  fputs(stderr_takes_colour() ? COLOUR_RED "frame-dsl:" COLOUR_RESET " " : "frame-dsl: ", stderr);
  va_list arguments;
  va_start(arguments, format);
  vfprintf(stderr, format, arguments);
  va_end(arguments);
  fputc('\n', stderr);
}

/* One diagnostic per line in the shape gcc prints, and nothing else -- no prefix, no summary. Every
 * editor already knows how to read this shape (flymake, Vim's errorformat, ALE, a VS Code problem
 * matcher), which is the whole reason to print it. It is --check's result, so it goes to stdout there,
 * plain, and in every other mode it is a problem, so it goes to stderr, where the severity is coloured
 * on a terminal. */
static void report(FILE *out, const FramedslResult *result) {
  bool colour = out == stderr && stderr_takes_colour();

  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);
    const char *name = diag->file != NULL ? diag->file : framedsl_source_name(result);
    bool warning = diag->severity == FRAMEDSL_SEVERITY_WARNING;
    const char *severity = warning ? "warning" : "error";
    const char *on = !colour ? "" : (warning ? COLOUR_YELLOW : COLOUR_RED);
    const char *off = colour ? COLOUR_RESET : "";

    if (diag->line > 0) {
      fprintf(out, "%s:%d:%d: %s%s:%s %s", name, diag->line, diag->column, on, severity, off, diag->message);
    } else {
      fprintf(out, "%s: %s%s:%s %s", name, on, severity, off, diag->message);
    }
    if (diag->check > 0) {
      fprintf(out, " [check %d]", diag->check);
    }
    fputc('\n', out);
    /* The fix, where there is one, as a note of its own: the same `file:line:col:` shape, but no
     * editor's pattern for this output takes a note for a problem. */
    if (diag->fix_title != NULL) {
      fprintf(out, "%s:%d:%d: note: %s\n", name, diag->fix_line, diag->fix_column, diag->fix_title);
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
    report(stdout, result);

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

/* The name enum `enumeration` gives `value`: what a fixed value is written as in the file. */
static const char *enum_name_of(const FramedslDefinition *definition, const char *enumeration, uint64_t value) {
  size_t index = 0;
  if (framedsl_find_enum(definition, enumeration, &index)) {
    for (size_t i = 0; i < framedsl_enum_item_count(definition, index); ++i) {
      const char *name = NULL;
      uint64_t listed = 0;
      if (framedsl_enum_item(definition, index, i, &name, &listed) && listed == value) {
        return name;
      }
    }
  }
  return "?";
}

static void describe(const FramedslDefinition *definition) {
  for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
    size_t fields = framedsl_field_count(definition, p);

    printf(
      "pkt %s: %zu field%s, %llu bits reserved\n",
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
      /* A conditional field (#12) says what it is present under, at the end of its line. */
      char when[512] = "";
      if (field.condition != NULL) {
        snprintf(when, sizeof(when), ", if %s", field.condition);
      }

      if (field.element_bits > 0) {
        /* A repetition (#55): its elements' fields, each at its place inside an element. */
        if (field.variable) {
          printf(
            "    %-14s %s, repeated, up to %llu elements of %llu bits%s\n",
            field.name,
            offset,
            (unsigned long long)(field.max_bits / field.element_bits),
            (unsigned long long)field.element_bits,
            when
          );
        } else {
          printf(
            "    %-14s %s, repeated, %llu elements of %llu bits%s\n",
            field.name,
            offset,
            (unsigned long long)(field.width_bits / field.element_bits),
            (unsigned long long)field.element_bits,
            when
          );
        }
        for (size_t m = 0; m < field.member_count; ++m) {
          FramedslField member;
          if (framedsl_member(definition, p, f, m, &member)) {
            printf(
              "      .%-12s at +%llu, %llu bits\n",
              member.name,
              (unsigned long long)member.offset_bits,
              (unsigned long long)member.width_bits
            );
          }
        }
      } else if (field.variable) {
        /* Reported in the unit it was written in, so the line can be compared with the file. */
        printf(
          "    %-14s %s, variable, reserves %llu bytes%s\n",
          field.name,
          offset,
          (unsigned long long)(field.max_bits / 8),
          when
        );
      } else if (field.has_fixed || field.has_default) {
        const uint64_t value = field.has_fixed ? field.fixed : field.default_value;
        printf(
          "    %-14s %s, %llu bits %s %#llx%s%s%s%s\n",
          field.name,
          offset,
          (unsigned long long)field.width_bits,
          field.has_fixed ? ":=" : "?=",
          (unsigned long long)value,
          field.enumeration != NULL ? " (" : "",
          field.enumeration != NULL ? enum_name_of(definition, field.enumeration, value) : "",
          field.enumeration != NULL ? ")" : "",
          when
        );
      } else {
        printf(
          "    %-14s %s, %llu bits%s%s%s%s%s\n",
          field.name,
          offset,
          (unsigned long long)field.width_bits,
          field.cal != NULL ? ", cal " : "",
          field.cal != NULL ? field.cal : "",
          field.enumeration != NULL ? ", enum " : "",
          field.enumeration != NULL ? field.enumeration : "",
          when
        );
      }
    }
  }
}

/* --list-packets: the packet names, one per line, for a completion script (#85) or any other program
 * to read. Nothing else on stdout or stderr -- a completion that printed a diagnostic would print it
 * into the middle of somebody's command line -- so a file that does not load is only an exit status
 * of 1. */
static int list_packets_of(const char *path, const char *stdin_name) {
  FramedslResult *result = load_path(path, stdin_name);
  if (result == NULL) {
    return 2;
  }
  const FramedslDefinition *definition = framedsl_definition(result);
  int status = 0;
  if (!framedsl_ok(result) || definition == NULL) {
    status = 1;
  } else {
    for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
      printf("%s\n", framedsl_packet_name(definition, p));
    }
  }
  framedsl_result_free(result);
  return status;
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
        fail("hex: byte %zu has only one digit (at offset %zu)", count, (size_t)(at - text));
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
      fail("hex: '%c' is not a hex digit (at offset %zu)", *at, (size_t)(at - text));
      return false;
    }

    if (high < 0) {
      high = digit;
      continue;
    }
    if (count >= capacity) {
      fail("hex: more than %d bytes", MAX_CAPTURE);
      return false;
    }
    out[count++] = (uint8_t)((high << 4) | digit);
    high = -1;
  }

  if (high >= 0) {
    fail("hex: an odd number of digits; the last byte is incomplete");
    return false;
  }
  if (count == 0 && !allow_empty) {
    fail("hex: no bytes to decode");
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

/* One field's value, as report_frame() prints it: a number (and its engineering value), or bytes. */
static void print_value(
  FILE *out,
  const char *label,
  const FramedslFrame *frame,
  FramedslFieldId id,
  const FramedslField *field
) {
  if (field->is_scalar) {
    uint64_t value = 0;
    double eng = 0;
    if (framedsl_get_u64(frame, id, &value) != FRAMEDSL_OK) {
      return;
    }
    /* A field typed by an enum shows its name -- or that it has none, which is the reading somebody
     * debugging a device most wants to see, so it is marked rather than left out (#11). */
    if (field->enumeration != NULL) {
      const char *name = NULL;
      bool listed = framedsl_get_name(frame, id, &name) == FRAMEDSL_OK;
      fprintf(
        out,
        "    %-14s %llu (%#llx) = %s [%s]\n",
        label,
        (unsigned long long)value,
        (unsigned long long)value,
        listed ? name : "<unlisted>",
        field->enumeration
      );
      return;
    }
    /* A calibrated field shows both: the raw value is what is on the wire, and the engineering
     * value is what anybody reading this actually wants to know. */
    if (field->cal != NULL && framedsl_get_eng(frame, id, &eng) == FRAMEDSL_OK) {
      fprintf(
        out,
        "    %-14s %llu (%#llx) = %.10g [%s]\n",
        label,
        (unsigned long long)value,
        (unsigned long long)value,
        eng,
        field->cal
      );
    } else {
      fprintf(out, "    %-14s %llu (%#llx)\n", label, (unsigned long long)value, (unsigned long long)value);
    }
    return;
  }

  /* A bit string, including every valof()-sized field. The length is what arrived, not what the
   * field reserved -- which is the whole point of asking the frame rather than the definition.
   * Nothing is copied: this tool only prints the bytes, and they are already in the frame. */
  size_t length = 0;
  char text[128] = "";

  const uint8_t *bytes = framedsl_get_bytes(frame, id, &length, NULL);
  if (bytes != NULL) {
    format_bytes(bytes, length, text, sizeof(text));
    fprintf(out, "    %-14s [%zu byte%s] %s\n", label, length, length == 1 ? "" : "s", text);
  }
}

/* Prints every field of the packet that was decoded, to `out`. */
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
    /* A field this frame did not carry (#12) is shown as such: that it exists and was not sent is
     * the reading, and leaving it out would look like a field the packet does not have. */
    bool present = true;
    if (framedsl_present(frame, id, &present) == FRAMEDSL_OK && !present) {
      fprintf(out, "    %-14s <absent>\n", field.name);
      continue;
    }
    if (field.element_bits == 0) {
      print_value(out, field.name, frame, id, &field);
      continue;
    }

    /* A repetition (#55): each element's fields, by the name framedsl_find() takes. */
    size_t count = 0;
    if (framedsl_count(frame, id, &count) != FRAMEDSL_OK) {
      continue;
    }
    fprintf(out, "    %-14s %zu element%s\n", field.name, count, count == 1 ? "" : "s");
    for (size_t e = 0; e < count; ++e) {
      for (size_t m = 0; m < field.member_count; ++m) {
        FramedslField member;
        FramedslFieldId element;
        char label[256];
        char dotted[512];
        if (!framedsl_member(definition, packet, f, m, &member)) {
          continue;
        }
        snprintf(label, sizeof(label), "%s[%zu].%s", field.name, e, member.name);
        snprintf(dotted, sizeof(dotted), "%s.%s", name, label);
        if (framedsl_find(definition, dotted, &element)) {
          print_value(out, label, frame, element, &member);
        }
      }
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
    fail("out of memory");
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
      fail("at byte %zu: %s", offset, decode_problem(why));
      status = 1;
      break;
    }

    report_frame(stdout, definition, frame, name, consumed);
    offset += consumed;
    count += 1;
  }

  if (status == 0) {
    printf("%zu frame%s, %zu byte%s\n", count, count == 1 ? "" : "s", length, length == 1 ? "" : "s");
  }

  framedsl_frame_free(frame);
  return status;
}

/* The calibrations and enums as a C header, on stdout, for a program that wants them compiled in. */
static int emit_c(const FramedslDefinition *definition) {
  size_t length = framedsl_emit_c(definition, NULL, NULL, 0);
  char *header = malloc(length + 1);
  if (length == 0 || header == NULL) {
    free(header);
    fail("the calibrations and enums could not be written as C");
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
  OPTION_EMIT_C,
  OPTION_CHECK,
  OPTION_WERROR,
  OPTION_FORMAT,
  OPTION_STDIN_NAME,
  OPTION_LSP,
  OPTION_LIST_PACKETS,
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
  { OPTION_DECODE,       "-d", "--decode",       "<hex>",  "decode bytes into packets and their fields"         },
  { OPTION_EMIT_C,       NULL, "--emit-c",       NULL,     "write the calibrations and enums to stdout as C"    },
  { OPTION_CHECK,        NULL, "--check",        NULL,     "only report problems; with --format, write nothing" },
  { OPTION_WERROR,       NULL, "--werror",       NULL,     "with --check: fail on a warning too"                },
  { OPTION_FORMAT,       NULL, "--format",       NULL,     "rewrite files in place, in the standard layout"     },
  { OPTION_STDIN_NAME,   NULL, "--stdin-name",   "<path>", "the file '-' stands for, for messages and imports"  },
  { OPTION_LSP,          NULL, "--lsp",          NULL,     "be a language server on stdin and stdout"           },
  { OPTION_LIST_PACKETS, NULL, "--list-packets", NULL,     "print the packet names, one per line"               },
  { OPTION_HELP,         "-h", "--help",         NULL,     "print this help and exit"                           },
  { OPTION_VERSION,      NULL, "--version",      NULL,     "print the version and exit"                         },
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

/* Plain text on stdout: this is the answer to a question, not a problem, and it has
 * to read the same piped into a pager, a file or a completion script. */
static void help(void) {
  fputs(
    "frame-dsl: check, describe, format and decode against a Frame-DSL definition.\n"
    "\n"
    "usage:\n"
    "  frame-dsl <file.fdsl>                                describe its packets and fields\n"
    "  frame-dsl <file.fdsl> -d <hex>                       decode bytes\n"
    "  frame-dsl <file.fdsl> --emit-c                       write the calibrations and enums out as C\n"
    "  frame-dsl <file.fdsl> --list-packets                 print the packet names, one per line\n"
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
    printf("  %-26s  %s\n", flags, option->help);
  }

  fputs(
    "\n"
    "arguments:\n"
    "  <file.fdsl>   a definition file; '-' reads it from standard input\n"
    "  <hex>         bytes as hex, one argument, separated any way or not at all: \"AA BB\", AA:BB, AABB\n"
    "\n"
    "exit status:\n"
    "  0  done, and nothing was wrong\n"
    "  1  the definition or the bytes are wrong; --check found an error (or, with\n"
    "     --werror, a warning); --format --check found a file that would change\n"
    "  2  the command line is wrong, a file cannot be read, or the input is not what it claims to be\n"
    "\n"
    "examples:\n"
    "  frame-dsl examples/uart_device.fdsl\n"
    "  frame-dsl examples/uart_device.fdsl -d \"81 00 07 01\"\n"
    "  frame-dsl examples/*.fdsl --check --werror\n"
    "  frame-dsl examples/*.fdsl --format --check\n"
    "  frame-dsl - --check --stdin-name src/device.fdsl < unsaved-buffer\n",
    stdout
  );
}

/* A mistake on the command line gets one line saying what it was, and a pointer to the rest --
 * not the whole help text, which buries the one line that matters. Plain and on stderr, the way
 * every other command-line tool says it. */
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
  bool emit = false;
  bool list_packets = false;

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

  /* Every definition file named; more than one only means something to --check and --format. */
  const char **paths = calloc((size_t)argc, sizeof(*paths));
  size_t path_count = 0;
  if (paths == NULL) {
    fail("out of memory");
    return 2;
  }

  /* Flags may come before or after the file. Nothing here needs an argument parser: anything that is
   * not a flag or a file is a usage error rather than something to guess at -- `field=value` included,
   * which is what encoding took before it left the command line (#97). */
  for (int i = 1; i < argc; ++i) {
    const Option *option = find_option(argv[i]);

    if (option == NULL) {
      if ((argv[i][0] != '-' || strcmp(argv[i], "-") == 0) && strchr(argv[i], '=') == NULL) {
        paths[path_count++] = argv[i];
      } else {
        char reason[256];
        snprintf(reason, sizeof(reason), "unexpected argument '%s'", argv[i]);
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
        free(paths);
        return usage_error(argv[0], reason);
      }
      value = argv[++i];
    }

    switch (option->id) {
    case OPTION_DECODE:
      hex = value;
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
    case OPTION_LIST_PACKETS:
      list_packets = true;
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
  if (list_packets && (lsp || check || format || emit || hex != NULL || werror)) {
    conflict = "--list-packets only prints the packet names; it does nothing else";
  } else if (lsp && (path_count > 0 || check || format || emit || hex != NULL || werror)) {
    conflict = "--lsp talks to an editor on stdin and stdout, and takes nothing else";
  } else if (emit && hex != NULL) {
    conflict = "--emit-c writes the calibrations and enums out; it does not decode as well";
  } else if (format && (emit || hex != NULL)) {
    conflict = "--format rewrites the definition; it does not decode or emit as well";
  } else if (format && werror) {
    conflict = "--werror is about warnings, and --format does not report any";
  } else if (check && (emit || hex != NULL)) {
    conflict = "--check only reports on the definition; it does not decode or emit as well";
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
    free(paths);
    return usage_error(argv[0], conflict);
  }

  /* Before the check below: the server is told what to work on by the editor, not by an argument. */
  if (lsp) {
    free(paths);
    return lsp_serve(stdin, stdout);
  }

  if (path == NULL) {
    free(paths);
    return usage_error(argv[0], "no definition file; usage: frame-dsl <file.fdsl> [options], or frame-dsl --lsp");
  }

  if (list_packets) {
    int status = list_packets_of(path, stdin_name);
    free(paths);
    return status;
  }

  if (format) {
    int status = 0;
    for (size_t i = 0; i < path_count; ++i) {
      int one = format_one(paths[i], stdin_name, check);
      status = one > status ? one : status;
    }
    free(paths);
    return status;
  }

  if (check) {
    int status = check_files(paths, path_count, stdin_name, werror);
    free(paths);
    return status;
  }

  FramedslResult *result = load_path(path, stdin_name);
  free(paths);
  if (result == NULL) {
    fail("out of memory");
    return 2;
  }

  int status;
  switch (framedsl_status(result)) {
  case FRAMEDSL_OK: {
    const FramedslDefinition *definition = framedsl_definition(result);

    /* Not ahead of --emit-c, whose standard output is C and must stay nothing but C. */
    if (!emit) {
      report(stderr, result);
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

    describe(definition);
    printf(
      "%s: ok, %zu packet%s\n",
      framedsl_source_name(result),
      framedsl_packet_count(definition),
      framedsl_packet_count(definition) == 1 ? "" : "s"
    );
    status = 0;
    break;
  }
  case FRAMEDSL_ERROR_INVALID:
    report(stderr, result);
    status = 1;
    break;
  /* A missing file is not a malformed file, and a script should not have to grep the message to
   * tell them apart. */
  case FRAMEDSL_ERROR_IO:
  case FRAMEDSL_ERROR_MEMORY:
  default:
    report(stderr, result);
    status = 2;
    break;
  }

  framedsl_result_free(result);
  return status;
}
