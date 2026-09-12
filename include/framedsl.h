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

/* Encoding reads a packet's scalars into a fixed array so that a width expression can index them
 * without allocating. A packet with more fields than this is refused rather than allocating for it;
 * nothing resembling a real frame comes close. */
#define FRAMEDSL_MAX_FIELDS_PER_PACKET 256

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
  /* Nothing in this definition matches the frame. Usually noise on the link, or a sender running a
   * newer definition than this one. */
  FRAMEDSL_ERROR_UNRECOGNISED,
  /* A packet matched and then the bytes ran out. Usually a framing bug in the caller's transport --
   * a different problem from an unrecognised frame, which is why it is a different value. */
  FRAMEDSL_ERROR_TRUNCATED,
  /* The field is real, and this frame is not holding it: a field of another packet after a decode,
   * or any field after a decode that failed. Separate from FRAMEDSL_ERROR_FIELD, which means the id
   * does not address a field at all -- one is a caller asking the wrong frame a reasonable question,
   * the other is a caller holding a broken id. */
  FRAMEDSL_ERROR_ABSENT,
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
 * unset: a field nobody has touched reads as zero and will encode as zero.
 *
 * A decoded frame is the exception, and it is not the same statement. See "What a frame holds"
 * below: values a caller writes are theirs to leave at zero, but a field that did not arrive did not
 * arrive, and reading it as zero would be inventing one. */

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

/* ---- What a frame holds ----------------------------------------------------------------------
 *
 * The arena has a slot for every field of every packet, which is what makes the whole thing one
 * allocation -- but a frame is only ever one packet at a time, and the other slots hold whatever was
 * last in them. A frame therefore tracks which of three things it is holding:
 *
 *   - **Whatever a caller put there.** A new frame, a reset one, and any frame a successful
 *     framedsl_set_*() has been called on. Every field reads, because the caller is building a frame
 *     and the values are theirs.
 *   - **A decoded packet.** After a successful framedsl_decode(). Its own fields read; a field of any
 *     other packet is FRAMEDSL_ERROR_ABSENT rather than a stale number from an earlier frame.
 *   - **Nothing.** After a decode that failed part-way. Every field is FRAMEDSL_ERROR_ABSENT, because
 *     the arena holds however far the walk got, which is not a frame.
 *
 * Writing to a field puts the frame back in the caller's hands: a decoded frame that a caller starts
 * setting fields on is no longer a record of what arrived, and refusing to read back a value the
 * caller just wrote would be the wrong way round.
 *
 * Encoding is not restricted by any of this. It names the packet it wants, so it is never the case of
 * a caller reading a field they did not mean to. */

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
 * there. 0 if the id does not address a bit-string field this frame is holding -- it has no status to
 * return, so a field the frame does not have answers the same as one holding nothing. Ask
 * framedsl_get_bytes() when the difference matters. */
size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id);

/* Builds a frame of `packet` from the values in `frame`, and returns it.
 *
 *     size_t length;
 *     FramedslStatus status;
 *     const uint8_t *bytes = framedsl_encode(frame, "TM_DATAGRAM_02", &length, &status);
 *     if (bytes != NULL) {
 *       send(socket, bytes, length, 0);
 *     }
 *
 * The bytes belong to the frame and are **valid until the next call on it**. That is the usual
 * shape -- encode, hand them to a socket, forget them -- and a caller who needs them to outlive that
 * copies them.
 *
 * `length` is in bytes and needs no companion bit count: a packet is a whole number of bytes or it
 * did not load.
 *
 * Fixed-value fields are written from the definition rather than from the arena, so a frame always
 * identifies itself whatever a caller put in those slots.
 *
 * NULL on failure, with the reason in `*status`: FRAMEDSL_ERROR_FIELD for a packet name this
 * definition does not have, FRAMEDSL_ERROR_RANGE for a width that overflows, exceeds what the field
 * reserved, or disagrees with the bytes set on the field it sizes. `status` may be NULL if the reason
 * is not wanted. Nothing is allocated, and on failure nothing is left for the caller to read. */
const uint8_t *framedsl_encode(FramedslFrame *frame, const char *packet, size_t *length,
                               FramedslStatus *status);

/* The same encode, writing into a buffer the caller already has rather than lending one.
 *
 *     uint8_t slot[64];
 *     size_t length;
 *     if (framedsl_encode_into(frame, "TM_DATAGRAM_02", slot, sizeof(slot), &length) == FRAMEDSL_OK) {
 *       send(socket, slot, length, 0);
 *     }
 *
 * For a caller who has somewhere the frame has to end up anyway -- a transmit ring, a region of a
 * larger message, a buffer whose lifetime is theirs rather than the frame's. framedsl_encode() is
 * still the one to reach for otherwise; this one exists because "valid until the next call" is an
 * answer some callers cannot use.
 *
 * `*length` always receives the size of the encoded frame, including when that is the reason the call
 * failed: too small a buffer is FRAMEDSL_ERROR_BUFFER with the size needed written there, so a caller
 * sizes the next attempt from the answer rather than from a guess. **Nothing is written to `buffer`
 * in that case** -- not a partial frame, not the bytes that would have fit.
 *
 * Every other failure is framedsl_encode()'s, for the same reasons. Nothing is allocated. */
FramedslStatus framedsl_encode_into(FramedslFrame *frame, const char *packet, uint8_t *buffer,
                                    size_t capacity, size_t *length);

/* Reads a frame out of `buffer` into `frame`, and returns the name of the packet it turned out to be.
 *
 *     size_t consumed;
 *     FramedslStatus status;
 *     const char *packet = framedsl_decode(frame, bytes, length, &consumed, &status);
 *     if (packet != NULL) {
 *       framedsl_get_u64(frame, len_id, &value);
 *     }
 *
 * Which packet it is is worked out from the frame itself: every packet carries at least one
 * fixed-value field (check 11) and no two can be confused (check 12), so the caller never says.
 *
 * The name points into the definition and lives as long as it does. It is the symmetric counterpart
 * of the name `framedsl_encode()` takes.
 *
 * `consumed` receives how many bytes the frame occupied, so a caller reading a stream can advance by
 * it and decode the next one. That matters because a packet with a `valof()` field has no length
 * until it has been decoded -- without this a caller would have to re-implement its length field by
 * hand. Bytes past the frame are left alone; `consumed` may be NULL.
 *
 * NULL on failure, with the reason in `*status`:
 *
 *   FRAMEDSL_ERROR_UNRECOGNISED  nothing here matches. Noise, or a sender ahead of this file.
 *   FRAMEDSL_ERROR_TRUNCATED     a packet matched and the bytes ran out.
 *   FRAMEDSL_ERROR_RANGE         a length field claims more than its field reserved.
 *
 * A failed decode leaves the arena holding whatever the walk had reached before it stopped, and the
 * frame holding nothing -- so every field reads back FRAMEDSL_ERROR_ABSENT rather than a value out of
 * a half-decoded frame. A caller who ignores the return value is told by the next call instead of
 * being handed a number. Nothing is read past `buffer + length`, and nothing is allocated. */
const char *framedsl_decode(FramedslFrame *frame, const uint8_t *buffer, size_t length,
                            size_t *consumed, FramedslStatus *status);

#ifdef __cplusplus
}
#endif
