#pragma once

/* Frame-DSL: load a packet-layout description at run time and ask what is in it.
 *
 * This is the whole public interface. Nothing else in the project is meant to be included, and this
 * header depends on nothing but the C standard library.
 *
 * The shape here answers three things the sketch in the original design did not:
 *
 *   - Nothing is global. A load returns a handle, so two definitions can be open at once and one
 *     program can speak two protocols.
 *   - A failed load is an object you interrogate, not a code you combine. Errors accumulated with
 *     `|=` turn two different failures into a third value meaning neither, and a caller that cannot
 *     tell which call failed cannot report it.
 *   - Fields are addressed by index, resolved once, rather than by string on every call.
 *
 * Encoding and decoding are not here yet: they need the arena and the bytecode, and those get
 * designed against a working encoder rather than guessed at now. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A field of this width or less is a scalar; wider is a bit string. The width alone decides. */
#define FRAMEDSL_SCALAR_MAX_BITS 64

typedef enum {
  FRAMEDSL_OK = 0,
  /* The file could not be read. Deliberately separate from FRAMEDSL_INVALID: a missing file is a
   * different problem from a malformed one, and a caller should not have to read the message. */
  FRAMEDSL_ERROR_IO,
  /* The definition was read and is wrong. framedsl_diagnostic() says how. */
  FRAMEDSL_ERROR_INVALID,
  FRAMEDSL_ERROR_MEMORY,
} FramedslStatus;

typedef enum {
  FRAMEDSL_BIT_ORDER_MSB_FIRST,
  FRAMEDSL_BIT_ORDER_LSB_FIRST,
} FramedslBitOrder;

typedef enum {
  FRAMEDSL_BYTE_ORDER_BIG_ENDIAN,
  FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN,
} FramedslByteOrder;

/* One problem with a definition. */
typedef struct {
  int line;
  int column;
  /* The numbered check from the design document, or 0 for a syntax error. Carried so a caller can
   * tell a file rejected for the right reason from one rejected for any reason. */
  int check;
  const char *message;
} FramedslDiagnostic;

typedef struct {
  const char *name;

  /* Width depends on another field's value, so it is only known per frame. */
  bool variable;
  /* The width in bits, when it is not variable. */
  uint64_t width_bits;
  /* The declared bound in bits, when it is. */
  uint64_t max_bits;
  /* What the arena reserves: the width, or the bound. */
  uint64_t reserved_bits;

  /* False for every field after a variable-width one -- they sit somewhere nobody knows until a
   * frame arrives, so offset_bits means nothing and is left at zero. */
  bool offset_known;
  uint64_t offset_bits;

  /* A scalar is read and written as an integer; anything else as bytes. */
  bool is_scalar;

  bool has_fixed;
  uint64_t fixed;

  /* Resolved: what applies after file, packet and field scope are combined -- not what was written. */
  FramedslBitOrder bit_order;
  FramedslByteOrder byte_order;
} FramedslField;

/* A loaded definition. Valid until its result is freed. */
typedef struct FramedslDefinition FramedslDefinition;

/* The outcome of a load: a definition, or the reasons there is not one. Never NULL except when even
 * the result could not be allocated. */
typedef struct FramedslResult FramedslResult;

/* Load a definition. The caller owns the result and must free it with framedsl_result_free().
 *
 * `name` is what diagnostics are attributed to; it is copied, and may be NULL. */
FramedslResult *framedsl_load_file(const char *path);
FramedslResult *framedsl_load_string(const char *source, const char *name);

void framedsl_result_free(FramedslResult *result);

FramedslStatus framedsl_status(const FramedslResult *result);
bool framedsl_ok(const FramedslResult *result);

/* The name the load was attributed to, for building messages. Never NULL. */
const char *framedsl_source_name(const FramedslResult *result);

/* Every problem found, not only the first: a file with four mistakes should take one run to fix.
 * Ordered by position. Valid until the result is freed. */
size_t framedsl_diagnostic_count(const FramedslResult *result);
const FramedslDiagnostic *framedsl_diagnostic(const FramedslResult *result, size_t index);

/* NULL unless the load succeeded. */
const FramedslDefinition *framedsl_definition(const FramedslResult *result);

size_t framedsl_packet_count(const FramedslDefinition *definition);

/* NULL if `index` is out of range. */
const char *framedsl_packet_name(const FramedslDefinition *definition, size_t index);

/* Bits the arena reserves for one packet. Returns 0 if `index` is out of range. */
uint64_t framedsl_packet_reserved_bits(const FramedslDefinition *definition, size_t index);

/* Writes the packet's index to `*index` and returns true; returns false if there is no such packet.
 * A miss is reported rather than answered with zero, which would be a valid index. */
bool framedsl_find_packet(const FramedslDefinition *definition, const char *name, size_t *index);

size_t framedsl_field_count(const FramedslDefinition *definition, size_t packet);

/* Copies the field's description into `*out`. False if either index is out of range, leaving `*out`
 * untouched. */
bool framedsl_field(const FramedslDefinition *definition, size_t packet, size_t field,
                    FramedslField *out);

bool framedsl_find_field(const FramedslDefinition *definition, size_t packet, const char *name,
                         size_t *index);

#ifdef __cplusplus
}
#endif
