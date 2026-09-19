#pragma once

/**
 * @file
 * @brief Frame-DSL: load a packet-layout description at run time and ask what is in it.
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
 * need the bytecode, which gets designed against a working encoder rather than guessed at now.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A field of this width or less is a scalar; wider is a bit string. The width alone decides. */
#define FRAMEDSL_SCALAR_MAX_BITS 64

/**
 * @brief Encoding reads a packet's scalars into a fixed array so that a width expression can index
 * them without allocating.
 *
 * A packet with more fields than this is refused rather than allocating for it; nothing resembling a
 * real frame comes close.
 */
#define FRAMEDSL_MAX_FIELDS_PER_PACKET 256

typedef enum {
  FRAMEDSL_OK = 0,
  /** @brief The file could not be read.
   *
   * Deliberately separate from FRAMEDSL_INVALID: a missing file is a different problem from a
   * malformed one, and a caller should not have to read the message.
   */
  FRAMEDSL_ERROR_IO,
  /** @brief The definition was read and is wrong. framedsl_diagnostic() says how. */
  FRAMEDSL_ERROR_INVALID,
  FRAMEDSL_ERROR_MEMORY,

  /* The runtime half. One enum rather than two, so a caller that prints a status has one function to
   * write, and so "which call failed" is answered by the call that returned non-OK. */

  /** @brief The field id does not address a field, addresses one belonging to another definition, or
   * addresses one of the wrong kind -- a bit string through the scalar accessors, or the reverse.
   */
  FRAMEDSL_ERROR_FIELD,
  /** @brief The value or length cannot fit the field: an integer wider than the field's bits, or a
   * byte count past what the field reserved.
   */
  FRAMEDSL_ERROR_RANGE,
  /** @brief A buffer the caller supplied is too small. The call reports the size it needed. */
  FRAMEDSL_ERROR_BUFFER,
  /** @brief Nothing in this definition matches the frame.
   *
   * Usually noise on the link, or a sender running a newer definition than this one.
   */
  FRAMEDSL_ERROR_UNRECOGNISED,
  /** @brief A packet matched and then the bytes ran out.
   *
   * Usually a framing bug in the caller's transport -- a different problem from an unrecognised
   * frame, which is why it is a different value.
   */
  FRAMEDSL_ERROR_TRUNCATED,
} FramedslStatus;

typedef enum {
  FRAMEDSL_BIT_ORDER_MSB_FIRST,
  FRAMEDSL_BIT_ORDER_LSB_FIRST,
} FramedslBitOrder;

typedef enum {
  FRAMEDSL_BYTE_ORDER_BIG_ENDIAN,
  FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN,
} FramedslByteOrder;

/** @brief One problem with a definition. */
typedef struct {
  int line;
  int column;
  /** @brief The numbered check from the design document, or 0 for a syntax error.
   *
   * Carried so a caller can tell a file rejected for the right reason from one rejected for any
   * reason.
   */
  int check;
  const char *message;
} FramedslDiagnostic;

typedef struct {
  const char *name;

  /** @brief Width depends on another field's value, so it is only known per frame. */
  bool variable;
  /** @brief The width in bits, when it is not variable. */
  uint64_t width_bits;
  /** @brief The reserved slot in bits, when it is.
   *
   * The definition writes this in bytes -- `data[128]:` -- because it sizes storage rather than a
   * position in a frame; it is reported here in bits so that every number on this struct shares one
   * unit.
   */
  uint64_t max_bits;
  /** @brief What the arena reserves: the width, or the bound. */
  uint64_t reserved_bits;

  /** @brief False for every field after a variable-width one -- they sit somewhere nobody knows
   * until a frame arrives, so offset_bits means nothing and is left at zero.
   */
  bool offset_known;
  uint64_t offset_bits;

  /** @brief A scalar is read and written as an integer; anything else as bytes. */
  bool is_scalar;

  bool has_fixed;
  uint64_t fixed;

  /** @brief Resolved: what applies after file, packet and field scope are combined -- not what was
   * written.
   */
  FramedslBitOrder bit_order;
  FramedslByteOrder byte_order;
} FramedslField;

/** @brief A loaded definition. Valid until its result is freed. */
typedef struct FramedslDefinition FramedslDefinition;

/**
 * @brief One frame's worth of values: an arena with a dedicated slot for every field of every packet
 * in a definition.
 *
 * Allocated once, never resized, and never allocating again for as long as it lives.
 */
typedef struct FramedslFrame FramedslFrame;

/**
 * @brief A field, resolved once from its name and cheap to pass around afterwards.
 *
 * Treat it as opaque. It is a struct rather than a size_t so that it can carry which definition it
 * came from: an id used against a frame of a different definition is then an error rather than a
 * plausible wrong field.
 */
typedef struct {
  uint32_t owner;
  uint16_t packet;
  uint16_t field;
} FramedslFieldId;

/** @brief The outcome of a load: a definition, or the reasons there is not one.
 *
 * Never NULL except when even the result could not be allocated.
 */
typedef struct FramedslResult FramedslResult;

/**
 * @brief Load a definition from a file.
 *
 * The caller owns the result and must free it with framedsl_result_free().
 *
 * @param path Path to the `.fdsl` source file.
 * @return The load's outcome. See FramedslResult.
 */
FramedslResult *framedsl_load_file(const char *path);

/**
 * @brief Load a definition from a string already in memory.
 *
 * The caller owns the result and must free it with framedsl_result_free().
 *
 * @param source The `.fdsl` source text.
 * @param name What diagnostics are attributed to; it is copied, and may be NULL.
 * @return The load's outcome. See FramedslResult.
 */
FramedslResult *framedsl_load_string(const char *source, const char *name);

void framedsl_result_free(FramedslResult *result);

FramedslStatus framedsl_status(const FramedslResult *result);
bool framedsl_ok(const FramedslResult *result);

/**
 * @brief The name the load was attributed to, for building messages.
 * @param result A result from framedsl_load_file() or framedsl_load_string().
 * @return The attributed name. Never NULL.
 */
const char *framedsl_source_name(const FramedslResult *result);

/**
 * @brief Number of problems found in a failed load. Every problem found, not only the first: a file
 * with four mistakes should take one run to fix.
 * @param result A result from framedsl_load_file() or framedsl_load_string().
 * @return How many diagnostics framedsl_diagnostic() can be asked for.
 */
size_t framedsl_diagnostic_count(const FramedslResult *result);

/**
 * @brief One problem with the load, by index. Ordered by position.
 * @param result A result from framedsl_load_file() or framedsl_load_string().
 * @param index An index below framedsl_diagnostic_count(result).
 * @return The diagnostic at `index`. Valid until the result is freed.
 */
const FramedslDiagnostic *framedsl_diagnostic(const FramedslResult *result, size_t index);

/**
 * @param result A result from framedsl_load_file() or framedsl_load_string().
 * @return The loaded definition, or NULL unless the load succeeded.
 */
const FramedslDefinition *framedsl_definition(const FramedslResult *result);

/* ---- Introspection ---------------------------------------------------------------------------
 *
 * Enumerate what a definition holds, rather than resolving one field by name -- for a caller that
 * does not yet know which field it wants: a tool listing every packet and field, or one printing a
 * decoded packet's fields after a decode. Meant to be used together: walk every packet with
 * framedsl_packet_count()/framedsl_packet_name(), and every field of one with
 * framedsl_field_count()/framedsl_field(); framedsl_find_packet()/framedsl_find_field() resolve the
 * same things by name instead of by index. framedsl_find() below is the shortcut once a caller
 * already knows exactly which field it wants. */

size_t framedsl_packet_count(const FramedslDefinition *definition);

/**
 * @param definition A loaded definition.
 * @param index An index below framedsl_packet_count(definition).
 * @return The packet's name, or NULL if `index` is out of range.
 */
const char *framedsl_packet_name(const FramedslDefinition *definition, size_t index);

/**
 * @param definition A loaded definition.
 * @param index An index below framedsl_packet_count(definition).
 * @return Bits the arena reserves for this packet. 0 if `index` is out of range.
 */
uint64_t framedsl_packet_reserved_bits(const FramedslDefinition *definition, size_t index);

/**
 * @brief Look up a packet by name.
 *
 * A miss is reported rather than answered with zero, which would be a valid index.
 *
 * @param definition A loaded definition.
 * @param name The packet's name.
 * @param index Set to the packet's index on success.
 * @return true if there is such a packet, false otherwise.
 */
bool framedsl_find_packet(const FramedslDefinition *definition, const char *name, size_t *index);

size_t framedsl_field_count(const FramedslDefinition *definition, size_t packet);

/**
 * @param definition A loaded definition.
 * @param packet An index below framedsl_packet_count(definition).
 * @param field An index below framedsl_field_count(definition, packet).
 * @param out Set to the field's description on success; untouched otherwise.
 * @return true if both indices are in range, false otherwise.
 */
bool framedsl_field(const FramedslDefinition *definition, size_t packet, size_t field, FramedslField *out);

bool framedsl_find_field(const FramedslDefinition *definition, size_t packet, const char *name, size_t *index);

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

/**
 * @param definition A loaded definition. Must outlive the frame.
 * @return A new frame, or NULL only if the allocation failed.
 */
FramedslFrame *framedsl_frame_new(const FramedslDefinition *definition);
void framedsl_frame_free(FramedslFrame *frame);

/**
 * @brief Back to the state framedsl_frame_new() returned: every slot zero, every length zero.
 *
 * Housekeeping rather than a rule -- nothing requires it, and a frame is usable for as long as you
 * keep it. It is here for the case where "start again, forget what this link has said so far" is
 * worth saying.
 */
void framedsl_frame_reset(FramedslFrame *frame);

/**
 * @brief Resolve a field from a dotted name, e.g. "TM_DATAGRAM_02.len".
 * @param definition A loaded definition.
 * @param dotted A "PACKET.field" name.
 * @param out Set to the field's id on success.
 * @return false if there is no such field, which includes a name with no dot in it.
 */
bool framedsl_find(const FramedslDefinition *definition, const char *dotted, FramedslFieldId *out);

/**
 * @brief The same id framedsl_find() would return, for a field already located by index.
 * @param definition A loaded definition.
 * @param packet An index below framedsl_packet_count(definition).
 * @param field An index below framedsl_field_count(definition, packet).
 * @param out Set to the field's id on success.
 * @return false if either index is out of range.
 */
bool framedsl_field_id(const FramedslDefinition *definition, size_t packet, size_t field, FramedslFieldId *out);

/* ---- What a frame holds ----------------------------------------------------------------------
 *
 * A frame remembers the last value of each packet, whether that came off the link or from you.
 *
 * The arena has a dedicated slot for every field of every packet, which is what makes the whole
 * thing one allocation. Nothing is shared between packets, so building a reply does not disturb the
 * command being answered, and one frame serves both directions of a conversation.
 *
 * Every slot starts at zero and holds whatever was last put in it. **There is no notion of a field
 * being unset**: a field nobody has written and no frame has carried reads back as zero, and encodes
 * as zero. That is one rule for values you wrote and values that arrived, rather than one each.
 *
 * Which packet arrived is framedsl_decode()'s return value, not something read back out of a field
 * afterwards -- so a zero is a zero and never has to be interrogated for whether it means anything.
 *
 * A decode that fails changes nothing. The walk puts the packet it is about to read aside first and
 * puts it back if it gives up, so a truncated or over-long frame leaves every field holding exactly
 * what it held before, rather than half of the frame that did not arrive.
 *
 * Encoding names the packet it wants and reads only that packet's slots. */

/**
 * @brief Set a scalar field: any field of FRAMEDSL_SCALAR_MAX_BITS or fewer.
 *
 * The value is checked against the field's declared width, so a 4-bit field set to 20 is
 * FRAMEDSL_ERROR_RANGE and the slot keeps what it had.
 *
 * There is one setter rather than four: a narrower one would add no safety the width check does not
 * already give, and the compiler widens the argument at the call site for free.
 *
 * @param frame The frame to write into.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @param value The value to store.
 * @return FRAMEDSL_OK, or the reason it could not be stored.
 */
FramedslStatus framedsl_set_u64(FramedslFrame *frame, FramedslFieldId id, uint64_t value);

/**
 * @brief Read a scalar field. See framedsl_set_u64() for what counts as scalar.
 * @param frame The frame to read from.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @param out Set to the field's value on success.
 * @return FRAMEDSL_OK, or the reason it could not be read.
 */
FramedslStatus framedsl_get_u64(const FramedslFrame *frame, FramedslFieldId id, uint64_t *out);

/**
 * @brief Set a bit-string field: everything wider than FRAMEDSL_SCALAR_MAX_BITS, including every
 * variable-width field.
 *
 * A fixed-width field takes exactly its own byte count -- its size is not negotiable, and accepting
 * fewer bytes would leave the rest of the field holding whatever was there before. A variable-width
 * field takes anything up to what it reserved, and remembers how much arrived.
 *
 * @param frame The frame to write into.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @param bytes The bytes to store.
 * @param length How many bytes `bytes` holds.
 * @return FRAMEDSL_OK, or the reason it could not be stored.
 */
FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id, const uint8_t *bytes, size_t length);

/**
 * @brief Read a bit-string field, without copying it. See framedsl_set_bytes() for what counts as a
 * bit string.
 *
 * The bytes are already in their final form in the frame's arena, so this hands back a pointer to
 * them rather than a copy -- the counterpart of framedsl_encode(), and the reason there is no buffer
 * to size or supply:
 *
 * @code
 *     size_t length;
 *     const uint8_t *data = framedsl_get_bytes(frame, data_id, &length, NULL);
 *     if (data != NULL) {
 *       write(fd, data, length);
 *     }
 * @endcode
 *
 * **The address does not change for the life of the frame.** The arena is allocated once and every
 * field has a fixed place in it, so this returns the same pointer every time. What changes is what
 * is behind it, and only when that field is written: framedsl_set_bytes() on it, or a *successful*
 * framedsl_decode() of the packet it belongs to. A decode of any other packet does not touch it, and
 * neither does a failed decode. framedsl_frame_reset() zeroes it. `*length` moves independently --
 * a shorter frame changes the length and not the address -- so a caller holding the pointer across a
 * decode must read the length again rather than assume it.
 *
 * Nothing is allocated and nothing is owned by the caller. Use framedsl_get_bytes_into() for a copy
 * that outlives the next frame.
 *
 * @param frame The frame to read from.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @param length Set to how many bytes are present. Required; a field holding nothing sets it to 0
 * and still returns a usable pointer, since NULL is reserved for a call that failed.
 * @param status Set to the reason for a NULL return: FRAMEDSL_ERROR_FIELD if the id does not address
 * a bit-string field, FRAMEDSL_ERROR_MEMORY for a NULL frame or `length`. May be NULL.
 * @return A pointer into the frame, or NULL on failure.
 */
const uint8_t *framedsl_get_bytes(
  const FramedslFrame *frame,
  FramedslFieldId id,
  size_t *length,
  FramedslStatus *status
);

/**
 * @brief Copy a bit-string field into a buffer of the caller's own.
 *
 * What to use when the bytes have to outlive the next frame, or when they are going somewhere that
 * needs to own them. Otherwise framedsl_get_bytes() hands back the same bytes without the copy.
 *
 * @param frame The frame to read from.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @param out Copies the bytes present into this buffer.
 * @param capacity Size of `out`.
 * @param length Set to how many bytes were copied, or, if `capacity` was too small, to the size that
 * would have been needed -- so a caller can size a buffer from the answer rather than from a guess.
 * Passing NULL and 0 for the buffer is how to ask for that size.
 * @return FRAMEDSL_OK, or FRAMEDSL_ERROR_BUFFER if `capacity` was too small.
 */
FramedslStatus framedsl_get_bytes_into(
  const FramedslFrame *frame,
  FramedslFieldId id,
  uint8_t *out,
  size_t capacity,
  size_t *length
);

/**
 * @brief Bytes the field currently holds: its fixed size, or how much the last framedsl_set_bytes()
 * put there.
 *
 * Mostly for sizing a buffer before framedsl_get_bytes_into(); framedsl_get_bytes() reports the same
 * number alongside the bytes themselves, so a caller reading the field has already been told.
 *
 * @param frame The frame to ask.
 * @param id A field id from framedsl_find() or framedsl_field_id().
 * @return The byte count, or 0 if the id does not address a bit-string field -- it has no status to
 * return, so a field the frame does not have answers the same as one holding nothing. Ask
 * framedsl_get_bytes() when the difference matters.
 */
size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id);

/**
 * @brief Builds a frame of `packet` from the values in `frame`, and returns it.
 *
 * @code
 *     size_t length;
 *     FramedslStatus status;
 *     const uint8_t *bytes = framedsl_encode(frame, "TM_DATAGRAM_02", &length, &status);
 *     if (bytes != NULL) {
 *       send(socket, bytes, length, 0);
 *     }
 * @endcode
 *
 * The bytes belong to the frame and are **valid until the next call on it**. That is the usual
 * shape -- encode, hand them to a socket, forget them -- and a caller who needs them to outlive that
 * copies them.
 *
 * Fixed-value fields are written from the definition rather than from the arena, so a frame always
 * identifies itself whatever a caller put in those slots.
 *
 * @param frame The frame to encode.
 * @param packet The packet to encode it as.
 * @param length Set to the encoded size, in bytes. Needs no companion bit count: a packet is a whole
 * number of bytes or it did not load.
 * @param status Set to the reason for a NULL return: FRAMEDSL_ERROR_FIELD for a packet name this
 * definition does not have, FRAMEDSL_ERROR_RANGE for a width that overflows, exceeds what the field
 * reserved, or disagrees with the bytes set on the field it sizes. May be NULL if the reason is not
 * wanted.
 * @return A pointer into the frame's own buffer, or NULL on failure. Nothing is allocated, and on
 * failure nothing is left for the caller to read.
 */
const uint8_t *framedsl_encode(FramedslFrame *frame, const char *packet, size_t *length, FramedslStatus *status);

/**
 * @brief The same encode, writing into a buffer the caller already has rather than lending one.
 *
 * @code
 *     uint8_t slot[64];
 *     size_t length;
 *     if (framedsl_encode_into(frame, "TM_DATAGRAM_02", slot, sizeof(slot), &length) == FRAMEDSL_OK) {
 *       send(socket, slot, length, 0);
 *     }
 * @endcode
 *
 * For a caller who has somewhere the frame has to end up anyway -- a transmit ring, a region of a
 * larger message, a buffer whose lifetime is theirs rather than the frame's. framedsl_encode() is
 * still the one to reach for otherwise; this one exists because "valid until the next call" is an
 * answer some callers cannot use.
 *
 * @param frame The frame to encode.
 * @param packet The packet to encode it as.
 * @param buffer The caller's buffer.
 * @param capacity Size of `buffer`.
 * @param length Always receives the size of the encoded frame, including when that is the reason the
 * call failed: too small a buffer is FRAMEDSL_ERROR_BUFFER with the size needed written here, so a
 * caller sizes the next attempt from the answer rather than from a guess. **Nothing is written to
 * `buffer` in that case** -- not a partial frame, not the bytes that would have fit.
 * @return FRAMEDSL_OK, or the same failures as framedsl_encode(). Nothing is allocated.
 */
FramedslStatus framedsl_encode_into(
  FramedslFrame *frame,
  const char *packet,
  uint8_t *buffer,
  size_t capacity,
  size_t *length
);

/**
 * @brief Reads a frame out of `buffer` into `frame`, and returns the name of the packet it turned out
 * to be.
 *
 * @code
 *     size_t consumed;
 *     FramedslStatus status;
 *     const char *packet = framedsl_decode(frame, bytes, length, &consumed, &status);
 *     if (packet != NULL) {
 *       framedsl_get_u64(frame, len_id, &value);
 *     }
 * @endcode
 *
 * The name points into the definition and is not the caller's to free or to size a buffer for.
 *
 * Which packet it is is worked out from the frame itself: every packet carries at least one
 * fixed-value field (check 11) and no two can be confused (check 12), so the caller never says.
 *
 * **A failed decode changes nothing.** The packet about to be walked is put aside first and put back
 * if the walk gives up, so every field still holds what it held before -- not half of the frame that
 * did not arrive. A caller who ignores the return value reads the previous frame's values rather than
 * a mixture of two. Nothing is read past `buffer + length`, and nothing is allocated.
 *
 * @param frame The frame to decode into.
 * @param buffer The bytes to decode.
 * @param length Size of `buffer`.
 * @param consumed Set to how many bytes the frame occupied, so a caller reading a stream can advance
 * by it and decode the next one -- which matters because a packet with a `valof()` field has no
 * length until it has been decoded. May be NULL.
 * @param status Set to the reason for a NULL return:
 *   - FRAMEDSL_ERROR_UNRECOGNISED nothing here matches. Noise, or a sender ahead of this file.
 *   - FRAMEDSL_ERROR_TRUNCATED a packet matched and the bytes ran out.
 *   - FRAMEDSL_ERROR_RANGE a length field claims more than its field reserved.
 * @return The packet's name, which points into the definition and lives as long as it does -- the
 * symmetric counterpart of the name framedsl_encode() takes, and not the caller's to free or to size
 * a buffer for. NULL on failure.
 */
const char *framedsl_decode(
  FramedslFrame *frame,
  const uint8_t *buffer,
  size_t length,
  size_t *consumed,
  FramedslStatus *status
);

#ifdef __cplusplus
}
#endif
