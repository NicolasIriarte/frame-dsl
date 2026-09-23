#pragma once

/**
 * @file
 * @brief Just enough JSON for the language server (#30): a reader into a tree, and a writer.
 *
 * Hand-written for the same reason the parser is: the project's point is writing these things, and a
 * JSON-RPC message is a small, well-specified input. The reader takes any JSON text (RFC 8259) and
 * refuses anything that is not; the tree lives in a Region and is freed with it. The writer is a text
 * builder that knows where commas go.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "region.h"

typedef enum {
  JSON_NULL,
  JSON_FALSE,
  JSON_TRUE,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
} JsonKind;

typedef struct Json Json;

struct Json {
  JsonKind kind;
  double number;
  /** @brief JSON_STRING: decoded to UTF-8 and terminated. `length` is in bytes, and may include a
   * `\u0000` the terminator does not see. */
  const char *string;
  size_t length;
  /** @brief JSON_ARRAY and JSON_OBJECT: the elements or members, in order. */
  Json **items;
  /** @brief JSON_OBJECT: the key of each member, parallel to `items`. */
  const char **keys;
  size_t count;
};

/**
 * @brief Reads `length` bytes of JSON text.
 * @return The value, allocated in `region`; NULL if the text is not JSON, is nested past a sane depth,
 * holds a number too large for a double, or an allocation failed.
 */
Json *json_parse(Region *region, const char *text, size_t length);

/** @brief The member `key` of an object, or NULL if `object` is not one or has no such member. */
const Json *json_get(const Json *object, const char *key);

/** @brief The string, or NULL if `value` is not a string. */
const char *json_string(const Json *value);

/** @brief Whether `value` is a number that is an integer, and if so which. */
bool json_integer(const Json *value, int64_t *out);

/* ---- Writing ------------------------------------------------------------------------------- */

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  /** @brief Set by any allocation failure; everything after it is a no-op. */
  bool failed;
  /** @brief Whether the container being written needs a comma before its next element. */
  bool comma[64];
  int depth;
} JsonWriter;

void json_writer_init(JsonWriter *writer);
void json_writer_free(JsonWriter *writer);

void json_object_begin(JsonWriter *writer);
void json_object_end(JsonWriter *writer);
void json_array_begin(JsonWriter *writer);
void json_array_end(JsonWriter *writer);

/** @brief The key of the next member of the object being written. */
void json_key(JsonWriter *writer, const char *key);

void json_write_string(JsonWriter *writer, const char *text, size_t length);
void json_write_cstring(JsonWriter *writer, const char *text);
void json_write_integer(JsonWriter *writer, int64_t value);
void json_write_bool(JsonWriter *writer, bool value);
void json_write_null(JsonWriter *writer);

/** @brief Writes `value` as it was read: what a response echoes back, such as a request's id. */
void json_write_value(JsonWriter *writer, const Json *value);
