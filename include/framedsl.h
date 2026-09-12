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
 * The runtime half is a frame: an arena with a dedicated slot for every field of every packet, which
 * you write values into and read them back out of. Encoding and decoding are not here yet -- they
 * need the bytecode, which gets designed against a working encoder rather than guessed at now. */

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

  /* The runtime half. One enum rather than two, so a caller that prints a status has one function to
   * write, and so "which call failed" is answered by the call that returned non-OK. */

  /* The field id does not address a field, addresses one belonging to another definition, or
   * addresses one of the wrong kind -- a bit string through the scalar accessors, or the reverse. */
  FRAMEDSL_ERROR_FIELD,
  /* The value or length cannot fit the field: an integer wider than the field's bits, or a byte
   * count past what the field reserved. */
  FRAMEDSL_ERROR_RANGE,
  /* A buffer the caller supplied is too small. The call reports the size it needed. */
  FRAMEDSL_ERROR_BUFFER,
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
  /* The reserved slot in bits, when it is. The definition writes this in bytes -- `data[128]:` --
   * because it sizes storage rather than a position in a frame; it is reported here in bits so that
   * every number on this struct shares one unit. */
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

/* One frame's worth of values: an arena with a dedicated slot for every field of every packet in a
 * definition. Allocated once, never resized, and never allocating again for as long as it lives. */
typedef struct FramedslFrame FramedslFrame;

/* A field, resolved once from its name and cheap to pass around afterwards.
 *
 * Treat it as opaque. It is a struct rather than a size_t so that it can carry which definition it
 * came from: an id used against a frame of a different definition is then an error rather than a
 * plausible wrong field. */
typedef struct {
  uint32_t owner;
  uint32_t packet;
  uint32_t field;
} FramedslFieldId;

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

/* ---- Frames ---------------------------------------------------------------------------------
 *
 * A frame holds the values. Create one from a definition, put values into it by field, read them
 * back. Everything it will ever need is allocated when it is created, so no call below allocates.
 *
 * The arena starts zeroed and stays whatever it was last set to. There is no notion of a field being
 * unset: a field nobody has touched reads as zero and will encode as zero. */

/* NULL only if the allocation failed. The definition must outlive the frame. */
FramedslFrame *framedsl_frame_new(const FramedslDefinition *definition);
void framedsl_frame_free(FramedslFrame *frame);

/* Back to the state framedsl_frame_new() returned: every slot zero, every length zero. */
void framedsl_frame_reset(FramedslFrame *frame);

/* Resolve a field from a dotted name -- "TM_DATAGRAM_02.len". False if there is no such field, which
 * includes a name with no dot in it. */
bool framedsl_find(const FramedslDefinition *definition, const char *dotted, FramedslFieldId *out);

/* The same id `framedsl_find()` would return, for a field already located by index. False if either
 * index is out of range. */
bool framedsl_field_id(const FramedslDefinition *definition, size_t packet, size_t field,
                       FramedslFieldId *out);

/* Scalars: any field of FRAMEDSL_SCALAR_MAX_BITS or fewer. The value is checked against the field's
 * declared width, so a 4-bit field set to 20 is FRAMEDSL_ERROR_RANGE and the slot keeps what it had.
 *
 * There is one setter rather than four: a narrower one would add no safety the width check does not
 * already give, and the compiler widens the argument at the call site for free. */
FramedslStatus framedsl_set_u64(FramedslFrame *frame, FramedslFieldId id, uint64_t value);
FramedslStatus framedsl_get_u64(const FramedslFrame *frame, FramedslFieldId id, uint64_t *out);

/* Bit strings: everything wider than FRAMEDSL_SCALAR_MAX_BITS, including every variable-width field.
 *
 * A fixed-width field takes exactly its own byte count -- its size is not negotiable, and accepting
 * fewer bytes would leave the rest of the field holding whatever was there before. A variable-width
 * field takes anything up to what it reserved, and remembers how much arrived. */
FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id, const uint8_t *bytes,
                                  size_t length);

/* Copies the bytes present into `out` and writes how many to `*length`. If `capacity` is too small,
 * returns FRAMEDSL_ERROR_BUFFER and writes the size needed to `*length` instead, so a caller can size
 * a buffer from the answer rather than from a guess. */
FramedslStatus framedsl_get_bytes(const FramedslFrame *frame, FramedslFieldId id, uint8_t *out,
                                  size_t capacity, size_t *length);

/* Bytes the field currently holds: its fixed size, or how much the last framedsl_set_bytes() put
 * there. 0 if the id does not address a bit-string field. */
size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id);

#ifdef __cplusplus
}
#endif
