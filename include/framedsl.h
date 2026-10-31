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
  /** @brief A field typed by an enum holds a value the enum does not list, or was given a name it does
   * not list.
   *
   * Reading one is not a failed decode: the frame arrived and the raw value is there to read. This is
   * how a device sending a state the definition does not know about is told apart from one that sent
   * nothing.
   */
  FRAMEDSL_ERROR_UNLISTED,
  /** @brief The field is conditional (#12), and its conditions do not hold over the values the frame
   * holds now: a decoded frame did not carry it, or a frame being built would leave it out.
   *
   * Reading one is refused rather than answered with whatever its slot holds, which is a value nobody
   * sent. framedsl_present() asks without reading.
   */
  FRAMEDSL_ERROR_ABSENT,
  /** @brief The field has a fixed value, so it cannot be set (#140).
   *
   * The encoder writes the definition's value whatever a program does, so a write is refused rather
   * than reported as done. Every write is, the value the field already has included: a loop copying
   * fields checks FramedslField.has_fixed.
   */
  FRAMEDSL_ERROR_FIXED,
} FramedslStatus;

typedef enum {
  FRAMEDSL_BIT_ORDER_MSB_FIRST,
  FRAMEDSL_BIT_ORDER_LSB_FIRST,
} FramedslBitOrder;

typedef enum {
  FRAMEDSL_BYTE_ORDER_BIG_ENDIAN,
  FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN,
} FramedslByteOrder;

/** @brief How bad a diagnostic is. */
typedef enum {
  /** @brief The definition is wrong, and the load failed because of it. */
  FRAMEDSL_SEVERITY_ERROR,
  /** @brief The definition loads, but probably does not say what was meant. */
  FRAMEDSL_SEVERITY_WARNING,
} FramedslSeverity;

/**
 * @brief One problem with a definition.
 *
 * Lines and columns count from 1, and a column counts bytes. A diagnostic with no position in the
 * file -- one that could not be read -- has line 0.
 */
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
  /** @brief The imported file the problem is in, or NULL when it is in the file that was loaded --
   * framedsl_source_name() names that one.
   */
  const char *file;
  FramedslSeverity severity;
  /**
   * @brief Just past the end of what the problem covers: the name or token at `line`:`column`, so an
   * editor can underline all of it. Equal to the start when there is nothing there to cover.
   */
  int end_line;
  int end_column;
  /**
   * @brief A suggested fix, or NULL when there is no single obvious one: replace `fix_line`:`fix_column`
   * up to (not including) `fix_end_line`:`fix_end_column` with `fix_text` -- an insertion when the two
   * are the same place, a deletion when `fix_text` is empty. Positions count as `line` and `column` do.
   *
   * `fix_title` says what it does, as a menu entry would: "change to 'byte_order'", "insert ';'",
   * "remove the unused block 'Spare'". A diagnostic in an imported file never carries one.
   */
  const char *fix_title;
  const char *fix_text;
  int fix_line;
  int fix_column;
  int fix_end_line;
  int fix_end_column;
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

  /** @brief The calibration `@cal:` attached to this field, or NULL. It points into the definition. */
  const char *cal;
  /** @brief Whether framedsl_set_eng() can work on this field: its curve is linear or declares an
   * `inverse`. False when there is no curve.
   */
  bool cal_invertible;

  /**
   * @brief For a repetition, `samples[48]: Sample[valof(count)];` (#55): the bits one element takes,
   * and how many fields it has -- framedsl_member() describes each. Zero for every other field, and for
   * a member itself.
   */
  uint64_t element_bits;
  size_t member_count;

  /** @brief The enum this field is typed by, `level: TxLevel;` (#11), or NULL. It points into the
   * definition; framedsl_find_enum() resolves it.
   */
  const char *enumeration;

  /** @brief For a conditional field (#12), what it is present under, as `valof(sec_hdr) == 1` -- the
   * conditions of nested groups joined by ` && `, outermost first, each spaced as the formatter spaces it
   * and an `||` in parentheses when there is more than one (#126). NULL for a field every frame carries.
   * It points into the definition.
   */
  const char *condition;
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
  /** @brief Which element of a repetition, for a member id; FRAMEDSL_NO_ELEMENT when none is chosen. */
  uint32_t element;
  /** @brief 0 for a whole field; for a field of a repeated block's element, its index plus one. */
  uint16_t member;
} FramedslFieldId;

/**
 * @brief The `element` of a member id that names no element yet, as framedsl_find() returns for
 * "PKT.samples.value". framedsl_element() picks one; the accessors refuse it until then.
 */
#define FRAMEDSL_NO_ELEMENT UINT32_MAX

/**
 * @brief A calibration, resolved once from its name by framedsl_find_cal().
 *
 * Treat it as opaque. Like FramedslFieldId it carries which definition it came from, so an id used
 * against another definition is an error rather than a different curve.
 */
typedef struct {
  uint32_t owner;
  uint32_t index;
} FramedslCalId;

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

/**
 * @brief Load a definition from a string, as though it were the file at `path`.
 *
 * Diagnostics are attributed to `path` and imports resolve from its directory, as
 * framedsl_load_file() would do -- but the text is `source`, not what is on disk. This is what an
 * editor needs: the buffer as typed, checked where it will be saved. `path` need not exist.
 *
 * The caller owns the result and must free it with framedsl_result_free().
 *
 * @param source The `.fdsl` source text.
 * @param path Where the text belongs; it is copied. NULL behaves as framedsl_load_string().
 * @return The load's outcome. See FramedslResult.
 */
FramedslResult *framedsl_load_source(const char *source, const char *path);

void framedsl_result_free(FramedslResult *result);

/**
 * @brief Format definition source in the project's one layout.
 *
 * Two-space indentation, no tabs, fields and settings aligned on their values, trailing comments
 * aligned, numbers spelled as written and comments kept word for word -- manual/language.md has the
 * rules. Formatting is idempotent, and the formatted text loads to the same definition.
 *
 * Only a file that parses is formatted; one that does not is refused, and loading it says why. Only
 * syntax matters here: a file that parses and then fails a check is formatted like any other.
 *
 * @param source The `.fdsl` source text.
 * @param out On FRAMEDSL_OK, the formatted text, NUL-terminated; the caller frees it with free().
 *     Untouched otherwise.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_INVALID when the source does not parse; FRAMEDSL_ERROR_MEMORY.
 */
FramedslStatus framedsl_format(const char *source, char **out);

FramedslStatus framedsl_status(const FramedslResult *result);
bool framedsl_ok(const FramedslResult *result);

/**
 * @brief The name the load was attributed to, for building messages.
 * @param result A result from framedsl_load_file() or framedsl_load_string().
 * @return The attributed name. Never NULL.
 */
const char *framedsl_source_name(const FramedslResult *result);

/**
 * @brief Number of problems found. Every problem found, not only the first: a file with four
 * mistakes should take one run to fix.
 *
 * A failed load has at least one error. A successful one may still have warnings, so a caller that
 * wants to show them asks here whatever the status.
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

/**
 * @brief Describe one field of a repetition's element (#55). `offset_bits` is its place inside an
 * element, which is always known; element i starts `i * element_bits` into the repetition.
 * @param definition A loaded definition.
 * @param packet An index below framedsl_packet_count(definition).
 * @param field A repetition's index: one whose FramedslField has a nonzero `element_bits`.
 * @param member An index below that FramedslField's `member_count`.
 * @param out Set to the member's description on success; untouched otherwise.
 * @return true if every index is in range and `field` is a repetition, false otherwise.
 */
bool framedsl_member(
  const FramedslDefinition *definition,
  size_t packet,
  size_t field,
  size_t member,
  FramedslField *out
);

/* ---- Frames ---------------------------------------------------------------------------------
 *
 * A frame holds the values. Create one from a definition, put values into it by field, read them
 * back. Everything it will ever need is allocated when it is created, so no call below allocates.
 *
 * The arena starts zeroed and stays whatever it was last set to. There is no notion of a field being
 * unset: a field nobody has touched reads as zero and will encode as zero. A fixed field reads its
 * fixed value.
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
 * @brief Back to the state framedsl_frame_new() returned: every slot zero but for fixed values, every
 * length zero.
 *
 * Housekeeping rather than a rule -- nothing requires it, and a frame is usable for as long as you
 * keep it. It is here for the case where "start again, forget what this link has said so far" is
 * worth saying.
 */
void framedsl_frame_reset(FramedslFrame *frame);

/**
 * @brief Resolve a field from a dotted name, e.g. "TM_DATAGRAM_02.len".
 *
 * A field of a repeated block's element (#55) is named through the repetition: "TM.samples[3].value"
 * for element 3, or "TM.samples.value" with no element chosen yet, for framedsl_element() to pick one
 * in a loop. "TM.samples" alone is the whole repetition, read and written as bytes.
 *
 * @param definition A loaded definition.
 * @param dotted A "PACKET.field" name.
 * @param out Set to the field's id on success.
 * @return false if there is no such field, which includes a name with no dot in it and an element
 * past what the repetition reserves.
 */
bool framedsl_find(const FramedslDefinition *definition, const char *dotted, FramedslFieldId *out);

/**
 * @brief Whether a field is in the frame (#12): whether a conditional field's conditions hold over the
 * values the frame holds now. A field with no conditions always is.
 *
 * Presence is worked out rather than stored, so it means the same for a decoded frame -- the fields it
 * carried -- and for one being built: the fields framedsl_encode() would write. A field that is not
 * present can still be set; the value waits in its slot until the condition holds. Reading it is
 * FRAMEDSL_ERROR_ABSENT.
 *
 * @param frame A frame.
 * @param id Any field id, a member of a repetition included -- present when its repetition is.
 * @param present Set on success.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if `id` does not address a field of this frame's definition.
 */
FramedslStatus framedsl_present(const FramedslFrame *frame, FramedslFieldId id, bool *present);

/**
 * @brief The same member of element `index` (#55): how a loop walks a repetition without building a
 * name for each element.
 *
 *     FramedslFieldId value, element;
 *     framedsl_find(definition, "TM_SAMPLES.samples.value", &value);
 *     framedsl_count(frame, value, &n);
 *     for (size_t i = 0; i < n; ++i) {
 *       framedsl_element(definition, value, i, &element);
 *       framedsl_get_u64(frame, element, &reading);
 *     }
 *
 * @param definition The definition `id` came from.
 * @param id A member id, with or without an element chosen.
 * @param index The element, from zero.
 * @param out Set to the member of that element on success.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if `id` is not a member of a repetition in this
 * definition; FRAMEDSL_ERROR_RANGE if `index` is past the most elements the repetition has room for.
 */
FramedslStatus framedsl_element(
  const FramedslDefinition *definition,
  FramedslFieldId id,
  size_t index,
  FramedslFieldId *out
);

/**
 * @brief How many elements a repetition holds in this frame (#55): the constant it was written with,
 * or what its count field says now -- as set by the caller, or as decoded.
 * @param frame A frame.
 * @param id The repetition, or any member of it.
 * @param count Set to the number of elements on success.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if `id` is neither; FRAMEDSL_ERROR_RANGE if the count
 * field holds more elements than the repetition reserves room for, or arithmetic on it fails.
 */
FramedslStatus framedsl_count(const FramedslFrame *frame, FramedslFieldId id, size_t *count);

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
 * FRAMEDSL_ERROR_RANGE and the slot keeps what it had. A field with a fixed value is
 * FRAMEDSL_ERROR_FIXED, whatever the value.
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

/* ---- Calibrations -------------------------------------------------------------------------------
 *
 * A calibration is a function the definition declares with `cal`, turning a field's raw value into
 * the engineering value somebody wants -- volts, degrees, metres -- and, where it can, back again.
 *
 * The arena holds the raw value only. A curve runs when an engineering value is asked for, so a field
 * with no curve, or one nobody reads through its curve, costs nothing, and framedsl_get_u64() still
 * reads the raw value of a calibrated field.
 *
 * Arithmetic is double throughout. A raw value above 2^53 does not survive the conversion exactly,
 * which no sensor reading approaches. */

/**
 * @brief A calibrated field's engineering value: its raw value through its curve.
 * @param frame The frame to read from.
 * @param id A scalar field with a `@cal:`.
 * @param out Set to the engineering value on success.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if the field has no curve or is not a scalar;
 * FRAMEDSL_ERROR_RANGE if the curve gave something that is not a finite number for this raw value --
 * a division by a value that happened to be zero, say.
 */
FramedslStatus framedsl_get_eng(const FramedslFrame *frame, FramedslFieldId id, double *out);

/**
 * @brief Set a calibrated field from an engineering value, through the inverse of its curve.
 *
 * A linear curve is inverted automatically; any other needs an `inverse` written in the definition.
 * The raw value the inverse gives is rounded to the nearest integer, which is the one place precision
 * is lost: reading the field back gives the engineering value of that integer, which is within half a
 * raw step of what was set. For a linear curve that is `|scale| / 2`.
 *
 * @param frame The frame to write into.
 * @param id A scalar field with a `@cal:`.
 * @param value The engineering value.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if the field has no curve, or one that cannot be inverted;
 * FRAMEDSL_ERROR_RANGE if the raw value it maps to is negative, not a number, or too wide for the
 * field. Nothing is written on failure.
 */
FramedslStatus framedsl_set_eng(FramedslFrame *frame, FramedslFieldId id, double value);

/** @brief How many calibrations the definition declares. */
size_t framedsl_cal_count(const FramedslDefinition *definition);

/** @brief A calibration's name, by index below framedsl_cal_count(), or NULL. */
const char *framedsl_cal_name(const FramedslDefinition *definition, size_t index);

/**
 * @brief Resolve a calibration by name, to call it without a field.
 * @param definition A loaded definition.
 * @param name The name after `cal` in the definition.
 * @param out Set to its id on success.
 * @return false if the definition has no calibration of that name.
 */
bool framedsl_find_cal(const FramedslDefinition *definition, const char *name, FramedslCalId *out);

/**
 * @brief Run a calibration forward: raw value in, engineering value out.
 *
 * @code
 *     FramedslCalId volts;
 *     double v;
 *     if (framedsl_find_cal(def, "volts", &volts) && framedsl_calibrate(def, volts, 2048, &v) == FRAMEDSL_OK) {
 *       printf("%.3f V\n", v);
 *     }
 * @endcode
 *
 * @param definition The definition the id came from.
 * @param cal An id from framedsl_find_cal().
 * @param raw The raw value.
 * @param eng Set to the result on success.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD for an id from another definition;
 * FRAMEDSL_ERROR_RANGE if the result is not a finite number.
 */
FramedslStatus framedsl_calibrate(const FramedslDefinition *definition, FramedslCalId cal, double raw, double *eng);

/**
 * @brief Run a calibration backward: engineering value in, raw value out, unrounded.
 * @param definition The definition the id came from.
 * @param cal An id from framedsl_find_cal().
 * @param eng The engineering value.
 * @param raw Set to the result on success. Not rounded -- that is framedsl_set_eng()'s business, since
 * only a field knows it holds an integer.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD for an id from another definition or a curve with no
 * inverse; FRAMEDSL_ERROR_RANGE if the result is not a finite number.
 */
FramedslStatus framedsl_uncalibrate(const FramedslDefinition *definition, FramedslCalId cal, double eng, double *raw);

/**
 * @brief Every calibration in the definition as C: a header of `static inline double` functions,
 * `<prefix><name>(double raw)` and, where there is one, `<prefix><name>_inverse(double eng)`. Every
 * enum follows, as `<prefix><name>_<NAME>` constants and a `<prefix><name>_name()` function from a
 * value to its name, NULL for one the enum does not list.
 *
 * For a program that wants a curve compiled in rather than interpreted. The functions are generated
 * from the compiled curves, not the source, and compute exactly what framedsl_calibrate() and
 * framedsl_uncalibrate() compute -- as long as the compiler keeps the arithmetic as written: no
 * `-ffast-math`, and no fused multiply-add contraction (`-ffp-contract=off`, the default under
 * `-std=c11`).
 *
 * snprintf-style: the whole header's length is returned whatever `capacity` was, and passing NULL and
 * 0 is how to ask it.
 *
 * @param definition A loaded definition.
 * @param prefix Put in front of every function name. NULL means "fdsl_".
 * @param out Written up to `capacity` and always terminated, when `capacity` is not zero.
 * @param capacity Size of `out`.
 * @return The header's length, not counting the terminator. 0 only if it could not be generated.
 */
size_t framedsl_emit_c(const FramedslDefinition *definition, const char *prefix, char *out, size_t capacity);

/* ---- Enumerations -----------------------------------------------------------------------------
 *
 * An enum names the values of a field typed by it, `level: TxLevel;`. The arena holds the number, as
 * for any field: framedsl_get_u64() and framedsl_set_u64() read and write it raw, and a raw value the
 * enum does not list is still a value -- a device reporting a state this definition has not heard of
 * is exactly what somebody debugging it wants to see. The calls below are the checked path, by name. */

/**
 * @brief A field's value, by the name its enum gives it.
 * @param frame The frame to read from.
 * @param id A field typed by an enum.
 * @param name Set to the name on success, pointing into the definition; to NULL on
 * FRAMEDSL_ERROR_UNLISTED.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if the field is not typed by an enum;
 * FRAMEDSL_ERROR_UNLISTED if the value it holds is not one the enum lists -- framedsl_get_u64() still
 * reads it.
 */
FramedslStatus framedsl_get_name(const FramedslFrame *frame, FramedslFieldId id, const char **name);

/**
 * @brief Set a field to the value its enum gives `name`.
 * @param frame The frame to write into.
 * @param id A field typed by an enum.
 * @param name One of the enum's names, as the definition writes it: `HIGH`, not `TxLevel.HIGH`.
 * @return FRAMEDSL_OK; FRAMEDSL_ERROR_FIELD if the field is not typed by an enum;
 * FRAMEDSL_ERROR_UNLISTED if the enum has no such name. Nothing is written on failure.
 */
FramedslStatus framedsl_set_name(FramedslFrame *frame, FramedslFieldId id, const char *name);

/** @brief How many enums the definition has, imported ones included. */
size_t framedsl_enum_count(const FramedslDefinition *definition);

/** @brief An enum's name, by index below framedsl_enum_count(), or NULL. An imported one is named
 * as the definition names it, `vendor.Mode` for one imported `as vendor`. */
const char *framedsl_enum_name(const FramedslDefinition *definition, size_t index);

/**
 * @brief Look up an enum by name -- the one FramedslField.enumeration gives, say.
 * @param definition A loaded definition.
 * @param name The enum's name.
 * @param index Set to its index on success.
 * @return false if the definition has no enum of that name.
 */
bool framedsl_find_enum(const FramedslDefinition *definition, const char *name, size_t *index);

/** @brief How many names an enum lists, by index below framedsl_enum_count(); 0 if out of range. */
size_t framedsl_enum_item_count(const FramedslDefinition *definition, size_t index);

/**
 * @brief One of an enum's names and its value. In the order written, which is increasing value.
 * @param definition A loaded definition.
 * @param index An index below framedsl_enum_count(definition).
 * @param item An index below framedsl_enum_item_count(definition, index).
 * @param name Set to the name, pointing into the definition. May be NULL.
 * @param value Set to its value. May be NULL.
 * @return false if either index is out of range.
 */
bool framedsl_enum_item(
  const FramedslDefinition *definition,
  size_t index,
  size_t item,
  const char **name,
  uint64_t *value
);

#ifdef __cplusplus
}
#endif
