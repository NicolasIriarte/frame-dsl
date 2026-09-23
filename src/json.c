#include "json.h"

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Reading --------------------------------------------------------------------------------- */

/* Deeper than any message the protocol sends by a wide margin, and shallow enough that a hostile
 * `[[[[...` cannot run the recursion off the stack. */
#define JSON_MAX_DEPTH 64

typedef struct {
  Region *region;
  const char *text;
  size_t length;
  size_t at;
  int depth;
} Reader;

static void skip_space(Reader *reader) {
  while (reader->at < reader->length) {
    char c = reader->text[reader->at];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
      return;
    }
    reader->at += 1;
  }
}

static bool literal(Reader *reader, const char *word) {
  size_t length = strlen(word);
  if (reader->length - reader->at < length || memcmp(reader->text + reader->at, word, length) != 0) {
    return false;
  }
  reader->at += length;
  return true;
}

static Json *new_value(Reader *reader, JsonKind kind) {
  Json *value = region_alloc(reader->region, sizeof(Json));
  if (value != NULL) {
    value->kind = kind;
  }
  return value;
}

static int hex_digit(char c) {
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

static bool read_hex4(Reader *reader, unsigned *out) {
  if (reader->length - reader->at < 4) {
    return false;
  }
  unsigned value = 0;
  for (int i = 0; i < 4; ++i) {
    int digit = hex_digit(reader->text[reader->at + (size_t)i]);
    if (digit < 0) {
      return false;
    }
    value = value * 16 + (unsigned)digit;
  }
  reader->at += 4;
  *out = value;
  return true;
}

static size_t put_utf8(char *out, unsigned code) {
  if (code < 0x80) {
    out[0] = (char)code;
    return 1;
  }
  if (code < 0x800) {
    out[0] = (char)(0xC0 | (code >> 6));
    out[1] = (char)(0x80 | (code & 0x3F));
    return 2;
  }
  if (code < 0x10000) {
    out[0] = (char)(0xE0 | (code >> 12));
    out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
    out[2] = (char)(0x80 | (code & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (code >> 18));
  out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((code >> 6) & 0x3F));
  out[3] = (char)(0x80 | (code & 0x3F));
  return 4;
}

/* A string, from just after its opening quote. Decoded into a region buffer no longer than the
 * encoded form, since no escape decodes to more bytes than it is written with. */
static char *read_string(Reader *reader, size_t *length) {
  /* Sized to this string alone -- up to its closing quote, stepping over escapes -- not to the rest of
   * the message, which for a message of many strings would add up to its length squared. */
  size_t end = reader->at;
  while (end < reader->length && reader->text[end] != '"') {
    end += reader->text[end] == '\\' ? 2 : 1;
  }
  if (end >= reader->length) {
    return NULL;
  }
  char *out = region_alloc(reader->region, end - reader->at + 1);
  if (out == NULL) {
    return NULL;
  }
  size_t used = 0;

  while (reader->at < reader->length) {
    unsigned char c = (unsigned char)reader->text[reader->at++];
    if (c == '"') {
      out[used] = '\0';
      *length = used;
      return out;
    }
    if (c < 0x20) {
      return NULL; /* control characters must be escaped */
    }
    if (c != '\\') {
      out[used++] = (char)c;
      continue;
    }
    if (reader->at >= reader->length) {
      return NULL;
    }
    char escape = reader->text[reader->at++];
    switch (escape) {
    case '"':
    case '\\':
    case '/':
      out[used++] = escape;
      break;
    case 'b':
      out[used++] = '\b';
      break;
    case 'f':
      out[used++] = '\f';
      break;
    case 'n':
      out[used++] = '\n';
      break;
    case 'r':
      out[used++] = '\r';
      break;
    case 't':
      out[used++] = '\t';
      break;
    case 'u': {
      unsigned code;
      if (!read_hex4(reader, &code)) {
        return NULL;
      }
      /* A surrogate pair is one code point written as two escapes. A lone half is not a character;
       * it becomes U+FFFD rather than bytes no UTF-8 reader would accept. */
      if (code >= 0xD800 && code <= 0xDBFF) {
        unsigned low;
        size_t before = reader->at;
        if (literal(reader, "\\u") && read_hex4(reader, &low) && low >= 0xDC00 && low <= 0xDFFF) {
          code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
        } else {
          reader->at = before;
          code = 0xFFFD;
        }
      } else if (code >= 0xDC00 && code <= 0xDFFF) {
        code = 0xFFFD;
      }
      used += put_utf8(out + used, code);
      break;
    }
    default:
      return NULL;
    }
  }
  return NULL; /* unterminated */
}

static bool read_number(Reader *reader, double *out) {
  size_t start = reader->at;
  if (reader->at < reader->length && reader->text[reader->at] == '-') {
    reader->at += 1;
  }
  /* The grammar, checked here, and the conversion left to strtod(): correctly rounding a decimal is not
   * something to write by hand. */
  if (reader->at >= reader->length) {
    return false;
  }
  if (reader->text[reader->at] == '0') {
    reader->at += 1;
  } else if (reader->text[reader->at] >= '1' && reader->text[reader->at] <= '9') {
    while (reader->at < reader->length && reader->text[reader->at] >= '0' && reader->text[reader->at] <= '9') {
      reader->at += 1;
    }
  } else {
    return false;
  }
  if (reader->at < reader->length && reader->text[reader->at] == '.') {
    reader->at += 1;
    size_t digits = reader->at;
    while (reader->at < reader->length && reader->text[reader->at] >= '0' && reader->text[reader->at] <= '9') {
      reader->at += 1;
    }
    if (reader->at == digits) {
      return false;
    }
  }
  if (reader->at < reader->length && (reader->text[reader->at] == 'e' || reader->text[reader->at] == 'E')) {
    reader->at += 1;
    if (reader->at < reader->length && (reader->text[reader->at] == '+' || reader->text[reader->at] == '-')) {
      reader->at += 1;
    }
    size_t digits = reader->at;
    while (reader->at < reader->length && reader->text[reader->at] >= '0' && reader->text[reader->at] <= '9') {
      reader->at += 1;
    }
    if (reader->at == digits) {
      return false;
    }
  }

  char buffer[64];
  size_t length = reader->at - start;
  if (length >= sizeof(buffer)) {
    return false;
  }
  memcpy(buffer, reader->text + start, length);
  buffer[length] = '\0';
  *out = strtod(buffer, NULL);
  /* `6e466` is written as JSON says a number may be, and is not a number a double can hold. Refused
   * rather than carried as an infinity: JSON has no way to write one back out, so a tree holding one
   * could not be re-serialised -- found by fuzzing. Underflow to zero is left alone; that is rounding,
   * which every number here is subject to. */
  return *out <= DBL_MAX && *out >= -DBL_MAX;
}

static Json *read_value(Reader *reader);

/* Elements or members, gathered into a growing array and copied into the region once the count is
 * known. */
static Json *read_container(Reader *reader, bool object) {
  Json *value = new_value(reader, object ? JSON_OBJECT : JSON_ARRAY);
  if (value == NULL) {
    return NULL;
  }
  const char close = object ? '}' : ']';

  size_t capacity = 0;
  Json **items = NULL;
  const char **keys = NULL;
  bool ok = false;

  skip_space(reader);
  if (reader->at < reader->length && reader->text[reader->at] == close) {
    reader->at += 1;
    return value;
  }

  for (;;) {
    const char *key = NULL;
    if (object) {
      skip_space(reader);
      if (reader->at >= reader->length || reader->text[reader->at] != '"') {
        break;
      }
      reader->at += 1;
      size_t key_length;
      key = read_string(reader, &key_length);
      skip_space(reader);
      if (key == NULL || reader->at >= reader->length || reader->text[reader->at] != ':') {
        break;
      }
      reader->at += 1;
    }

    Json *item = read_value(reader);
    if (item == NULL) {
      break;
    }
    if (value->count == capacity) {
      capacity = capacity == 0 ? 8 : capacity * 2;
      Json **grown_items = realloc(items, capacity * sizeof(*items));
      if (grown_items == NULL) {
        break;
      }
      items = grown_items;
      const char **grown_keys = realloc(keys, capacity * sizeof(*keys));
      if (grown_keys == NULL) {
        break;
      }
      keys = grown_keys;
    }
    items[value->count] = item;
    keys[value->count] = key;
    value->count += 1;

    skip_space(reader);
    if (reader->at >= reader->length) {
      break;
    }
    char c = reader->text[reader->at++];
    if (c == close) {
      ok = true;
      break;
    }
    if (c != ',') {
      break;
    }
  }

  if (ok) {
    value->items = region_alloc(reader->region, value->count * sizeof(*items));
    value->keys = object ? region_alloc(reader->region, value->count * sizeof(*keys)) : NULL;
    if (value->items == NULL || (object && value->keys == NULL)) {
      ok = false;
    } else {
      memcpy(value->items, items, value->count * sizeof(*items));
      if (object) {
        memcpy(value->keys, keys, value->count * sizeof(*keys));
      }
    }
  }
  free(items);
  free(keys);
  return ok ? value : NULL;
}

static Json *read_value(Reader *reader) {
  if (++reader->depth > JSON_MAX_DEPTH) {
    return NULL;
  }
  skip_space(reader);
  if (reader->at >= reader->length) {
    return NULL;
  }

  Json *value = NULL;
  char c = reader->text[reader->at];
  if (c == '{' || c == '[') {
    reader->at += 1;
    value = read_container(reader, c == '{');
  } else if (c == '"') {
    reader->at += 1;
    size_t length;
    const char *string = read_string(reader, &length);
    if (string != NULL && (value = new_value(reader, JSON_STRING)) != NULL) {
      value->string = string;
      value->length = length;
    }
  } else if (literal(reader, "true")) {
    value = new_value(reader, JSON_TRUE);
  } else if (literal(reader, "false")) {
    value = new_value(reader, JSON_FALSE);
  } else if (literal(reader, "null")) {
    value = new_value(reader, JSON_NULL);
  } else {
    double number;
    if (read_number(reader, &number) && (value = new_value(reader, JSON_NUMBER)) != NULL) {
      value->number = number;
    }
  }
  reader->depth -= 1;
  return value;
}

Json *json_parse(Region *region, const char *text, size_t length) {
  Reader reader = { .region = region, .text = text, .length = length };
  Json *value = read_value(&reader);
  skip_space(&reader);
  return value != NULL && reader.at == length ? value : NULL;
}

const Json *json_get(const Json *object, const char *key) {
  if (object == NULL || object->kind != JSON_OBJECT) {
    return NULL;
  }
  for (size_t i = 0; i < object->count; ++i) {
    if (strcmp(object->keys[i], key) == 0) {
      return object->items[i];
    }
  }
  return NULL;
}

const char *json_string(const Json *value) {
  return value != NULL && value->kind == JSON_STRING ? value->string : NULL;
}

bool json_integer(const Json *value, int64_t *out) {
  /* Within +-2^53 every integer is exactly a double, and the cast is defined; past it, neither holds. */
  if (value == NULL || value->kind != JSON_NUMBER || !(value->number >= -9007199254740992.0)
      || !(value->number <= 9007199254740992.0) || (double)(int64_t)value->number != value->number) {
    return false;
  }
  *out = (int64_t)value->number;
  return true;
}

/* ---- Writing --------------------------------------------------------------------------------- */

void json_writer_init(JsonWriter *writer) {
  memset(writer, 0, sizeof(*writer));
}

void json_writer_free(JsonWriter *writer) {
  free(writer->data);
  json_writer_init(writer);
}

static void raw(JsonWriter *writer, const char *text, size_t length) {
  if (writer->failed) {
    return;
  }
  if (writer->length + length + 1 > writer->capacity) {
    size_t capacity = writer->capacity == 0 ? 256 : writer->capacity;
    while (writer->length + length + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(writer->data, capacity);
    if (grown == NULL) {
      writer->failed = true;
      return;
    }
    writer->data = grown;
    writer->capacity = capacity;
  }
  memcpy(writer->data + writer->length, text, length);
  writer->length += length;
  writer->data[writer->length] = '\0';
}

/* Before any value: the comma, unless it is the first thing in its container or follows a key. */
static void element(JsonWriter *writer) {
  if (writer->depth > 0 && writer->comma[writer->depth]) {
    raw(writer, ",", 1);
  }
  writer->comma[writer->depth] = true;
}

static void container_open(JsonWriter *writer, const char *bracket) {
  element(writer);
  raw(writer, bracket, 1);
  if (writer->depth + 1 < (int)(sizeof(writer->comma) / sizeof(*writer->comma))) {
    writer->depth += 1;
    writer->comma[writer->depth] = false;
  } else {
    writer->failed = true;
  }
}

static void container_close(JsonWriter *writer, const char *bracket) {
  raw(writer, bracket, 1);
  if (writer->depth > 0) {
    writer->depth -= 1;
  }
}

void json_object_begin(JsonWriter *writer) {
  container_open(writer, "{");
}

void json_object_end(JsonWriter *writer) {
  container_close(writer, "}");
}

void json_array_begin(JsonWriter *writer) {
  container_open(writer, "[");
}

void json_array_end(JsonWriter *writer) {
  container_close(writer, "]");
}

static void quoted(JsonWriter *writer, const char *text, size_t length) {
  raw(writer, "\"", 1);
  size_t run = 0;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)text[i];
    const char *escape = NULL;
    char unicode[8];
    if (c == '"') {
      escape = "\\\"";
    } else if (c == '\\') {
      escape = "\\\\";
    } else if (c == '\n') {
      escape = "\\n";
    } else if (c == '\r') {
      escape = "\\r";
    } else if (c == '\t') {
      escape = "\\t";
    } else if (c < 0x20) {
      snprintf(unicode, sizeof(unicode), "\\u%04x", c);
      escape = unicode;
    }
    if (escape != NULL) {
      raw(writer, text + run, i - run);
      raw(writer, escape, strlen(escape));
      run = i + 1;
    }
  }
  raw(writer, text + run, length - run);
  raw(writer, "\"", 1);
}

void json_key(JsonWriter *writer, const char *key) {
  element(writer);
  quoted(writer, key, strlen(key));
  raw(writer, ":", 1);
  /* The value that follows is part of this member, not a new element needing a comma. */
  writer->comma[writer->depth] = false;
}

void json_write_string(JsonWriter *writer, const char *text, size_t length) {
  element(writer);
  quoted(writer, text, length);
}

void json_write_cstring(JsonWriter *writer, const char *text) {
  json_write_string(writer, text, strlen(text));
}

void json_write_integer(JsonWriter *writer, int64_t value) {
  element(writer);
  char buffer[32];
  int length = snprintf(buffer, sizeof(buffer), "%" PRId64, value);
  raw(writer, buffer, (size_t)length);
}

void json_write_bool(JsonWriter *writer, bool value) {
  element(writer);
  raw(writer, value ? "true" : "false", value ? 4 : 5);
}

void json_write_null(JsonWriter *writer) {
  element(writer);
  raw(writer, "null", 4);
}

void json_write_value(JsonWriter *writer, const Json *value) {
  if (value == NULL) {
    json_write_null(writer);
    return;
  }
  switch (value->kind) {
  case JSON_NULL:
    json_write_null(writer);
    break;
  case JSON_FALSE:
  case JSON_TRUE:
    json_write_bool(writer, value->kind == JSON_TRUE);
    break;
  case JSON_NUMBER: {
    int64_t integer;
    if (json_integer(value, &integer)) {
      json_write_integer(writer, integer);
    } else {
      element(writer);
      char buffer[40];
      int length = snprintf(buffer, sizeof(buffer), "%.17g", value->number);
      raw(writer, buffer, (size_t)length);
    }
    break;
  }
  case JSON_STRING:
    json_write_string(writer, value->string, value->length);
    break;
  case JSON_ARRAY:
    json_array_begin(writer);
    for (size_t i = 0; i < value->count; ++i) {
      json_write_value(writer, value->items[i]);
    }
    json_array_end(writer);
    break;
  case JSON_OBJECT:
    json_object_begin(writer);
    for (size_t i = 0; i < value->count; ++i) {
      json_key(writer, value->keys[i]);
      json_write_value(writer, value->items[i]);
    }
    json_object_end(writer);
    break;
  }
}
