# The C API

Everything a program needs is in [`include/framedsl.h`](../include/framedsl.h). It is the only header
to include, and it depends on nothing but the C standard library.

It covers the whole path: load a definition, ask what is in it, put values into a frame, encode it,
and decode one back. The reasoning behind the shapes is in [`docs/design.md`](../docs/design.md).

## The shortest useful program

```c
#include <stdio.h>
#include "framedsl.h"

int main(void) {
  FramedslResult *result = framedsl_load_file("frames.fdsl");

  if (!framedsl_ok(result)) {
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *d = framedsl_diagnostic(result, i);
      const char *where = d->file != NULL ? d->file : framedsl_source_name(result);
      fprintf(stderr, "%s:%d:%d: %s\n", where, d->line, d->column, d->message);
    }
    framedsl_result_free(result);
    return 1;
  }

  const FramedslDefinition *def = framedsl_definition(result);
  printf("%zu packets\n", framedsl_packet_count(def));

  framedsl_result_free(result);
  return 0;
}
```

## Three things the shape is answering

**Nothing is global.** A load returns a handle, so two definitions can be open at once and one
program can speak two protocols. The original design sketch had no handle, which quietly meant one
definition per process forever.

**A failed load is an object, not a code.** The sketch accumulated errors with `|=`, which turns two
different failures into a third value meaning neither -- and a caller that cannot tell *which* call
failed cannot report it. Here a load always returns a result: ask it whether it worked, and if not,
ask it what went wrong. Every problem is reported, not just the first.

**Fields are addressed by index.** Resolve a name once with `framedsl_find_field()` and use the index
after that, rather than a string lookup on every call.

## Loading

```c
FramedslResult *framedsl_load_file(const char *path);
FramedslResult *framedsl_load_string(const char *source, const char *name);
FramedslResult *framedsl_load_source(const char *source, const char *path);
void framedsl_result_free(FramedslResult *result);
```

`name` is what diagnostics are attributed to, and may be `NULL`. `framedsl_load_string()` resolves
imports from the working directory, since a string has no directory of its own;
`framedsl_load_source()` loads the string *as the file at `path`* -- attributed to it, importing from
beside it -- which is what an editor checking an unsaved buffer needs. `path` does not have to exist. The caller owns the result. Freeing
it invalidates the definition, every diagnostic, and every string reached through either -- so a
field name that must outlive the result has to be copied.

`framedsl_result_free(NULL)` is safe, as is passing `NULL` to every accessor below: a caller whose
load failed is holding `NULL`, and finding out should not mean crashing.

## Did it work

```c
FramedslStatus framedsl_status(const FramedslResult *result);
bool framedsl_ok(const FramedslResult *result);
```

| Status | |
| --- | --- |
| `FRAMEDSL_OK` | loaded |
| `FRAMEDSL_ERROR_IO` | the file could not be read |
| `FRAMEDSL_ERROR_INVALID` | it was read, and it is wrong |
| `FRAMEDSL_ERROR_MEMORY` | allocation failed, or a handle you passed was `NULL` |
| `FRAMEDSL_ERROR_FIELD` | the field id does not address a field of this frame, or addresses one of the wrong kind |
| `FRAMEDSL_ERROR_RANGE` | the value or length does not fit the field |
| `FRAMEDSL_ERROR_BUFFER` | a buffer you supplied is too small; the call reports the size needed |
| `FRAMEDSL_ERROR_UNRECOGNISED` | nothing in the definition matches the frame you handed over |
| `FRAMEDSL_ERROR_TRUNCATED` | a packet matched and then the bytes ran out |
| `FRAMEDSL_ERROR_UNLISTED` | a field typed by an enum holds a value its enum does not list, or was given a name it does not list |
| `FRAMEDSL_ERROR_ABSENT` | a conditional field the frame does not hold: its `@if` does not hold over the frame's values |
| `FRAMEDSL_ERROR_FIXED` | a field with a fixed value cannot be set |

`IO` and `INVALID` are separate on purpose. A file that is not there is a different problem from a
file that is wrong, and a caller should not have to read the message to tell them apart.

Everything from `FIELD` on belongs to the frame half below. They are in the same enum rather than one of their
own, so a caller that prints a status has one function to write -- and "which call failed" never
needs asking, because it is the one that returned something other than `FRAMEDSL_OK`.

`FIELD` is the only one about the id itself: it is out of range, it belongs to another definition, or
it is being used through the wrong pair of accessors. **Reading is otherwise never refused** -- see
[what a frame holds](#what-a-frame-holds) below.

## What was wrong

```c
size_t framedsl_diagnostic_count(const FramedslResult *result);
const FramedslDiagnostic *framedsl_diagnostic(const FramedslResult *result, size_t index);
```

```c
typedef struct {
  int line, column;
  int check;             // the numbered check from docs/design.md, or 0 for a syntax error
  const char *message;
  const char *file;      // the imported file it is in, or NULL for the one that was loaded
  FramedslSeverity severity;       // FRAMEDSL_SEVERITY_ERROR or FRAMEDSL_SEVERITY_WARNING
  int end_line, end_column;        // just past the end of what it is about
} FramedslDiagnostic;
```

An **error** fails the load. A **warning** does not: the file loads, and the warning is there to be
read anyway -- so ask `framedsl_diagnostic_count()` whatever the status, not only after a failure.

Lines and columns count from 1, and a column counts bytes. The span from `line:column` to
`end_line:end_column` covers the token the problem is about -- a whole name, not its first letter --
which is what an editor underlines. It is empty (end equals start) when there is nothing there to
cover, and a problem with no position at all, a file that could not be read, has line 0.

Diagnostics come back in file order -- imported files first, since their problems stop the importing
file at its top. A problem inside an imported file carries that file's name in `file` and a position
in *that* file, so print `d->file ? d->file : framedsl_source_name(result)` in front of the line. **`check` is the field worth using in a test**: "the file was
rejected" is a weaker claim than "the file was rejected for the reason it should have been", and only
the second catches a checker that is right by accident.

## What is in it

```c
const FramedslDefinition *framedsl_definition(const FramedslResult *result);   // NULL unless ok

size_t      framedsl_packet_count(const FramedslDefinition *def);
const char *framedsl_packet_name(const FramedslDefinition *def, size_t packet);
uint64_t    framedsl_packet_reserved_bits(const FramedslDefinition *def, size_t packet);
bool        framedsl_find_packet(const FramedslDefinition *def, const char *name, size_t *index);

size_t framedsl_field_count(const FramedslDefinition *def, size_t packet);
bool   framedsl_field(const FramedslDefinition *def, size_t packet, size_t field, FramedslField *out);
bool   framedsl_find_field(const FramedslDefinition *def, size_t packet, const char *name, size_t *index);
```

A lookup that misses returns `false` rather than an index, because `0` is a perfectly good index and
would be indistinguishable from a hit. An out-of-range `framedsl_field()` leaves `*out` untouched
rather than zeroing it, so a caller who ignores the return value gets their own stale data instead of
a zeroed struct that looks like a real field.

### `FramedslField`

| Member | |
| --- | --- |
| `name` | as written in the definition |
| `variable` | width depends on another field, so it is only known per frame |
| `width_bits` | the width, when it is not variable (`0` when it is) |
| `max_bits` | the reserved slot **in bits**, when it is (`0` when it is not). The definition writes it in bytes, as `data[128]:` |
| `reserved_bits` | what the arena reserves: the width, or the bound |
| `kind` | how its bits read: `FRAMEDSL_KIND_UNSIGNED`, `_SIGNED` or `_FLOAT`. `fixed` and `default_value` hold the pattern whatever this says, so -1 in an `i8` is `0xFF` |
| `offset_known` | false for every field after a variable-width or a conditional one |
| `offset_bits` | where it starts, when that is known |
| `is_scalar` | 64 bits or fewer: read as an integer, not as bytes |
| `has_fixed`, `fixed` | the value that identifies the packet, if this field carries one |
| `bit_order`, `byte_order` | **resolved** |
| `cal`, `cal_invertible` | the calibration `@cal:` names, or `NULL`, and whether it can be set -- see [Calibrations](#calibrations) |
| `element_bits`, `member_count` | for a repetition, its element's size and fields; `0` otherwise |
| `enumeration` | the enum the field is typed by, or `NULL` -- see [Enumerations](#enumerations) |
| `condition` | what a conditional field is present under, as `valof(sec_hdr) == 1`, or `NULL` -- see [Conditional fields](#conditional-fields) |

"Resolved" matters: these are what actually applies to the field after file, packet and field scope
are combined -- not what was written at any one of them. A caller wants to know how the bits land,
not which scope happened to say so.

A variable field reports `width_bits == 0` and `variable == true` rather than a plausible-looking
number, and a field after one reports `offset_known == false` rather than an offset that would be
wrong for every frame but the shortest.

## Frames

A definition says what a packet looks like. A **frame** holds one packet's worth of values.

```c
FramedslFrame *framedsl_frame_new(const FramedslDefinition *definition);
void           framedsl_frame_free(FramedslFrame *frame);
void           framedsl_frame_reset(FramedslFrame *frame);
```

`framedsl_frame_new()` makes **one** allocation, holding a dedicated slot for every field of every
packet in the definition. Nothing below it allocates, ever -- that is the property the whole arena
design exists for, and there is a test that counts calls to the allocator rather than taking it on
trust.

The definition must outlive the frame. Several frames can share one definition, and they share
nothing else: two threads encoding different frames of the same protocol do not interact.

### There is no "unset"

The arena starts zeroed, and a slot keeps whatever was last written to it. A field nobody has touched
reads back as zero and will encode as zero, unless the definition gives it a default, when it reads and
encodes that. A fixed field reads its fixed value, until a decode replaces that with what arrived.
`framedsl_frame_reset()` puts it all back to that state.

There is no call to put one field back. `FramedslField.default_value`, in the bits `fixed` uses, says what
to write.

This is a deliberate choice rather than an omission. Tracking which fields a caller had set would
mean a bit per field, a rule about what `reset` does to it, and an encode that fails on a field the
caller quite reasonably wanted to leave at zero.

### What a frame holds

**A frame remembers the last value of each packet, and whether that came off the link or from you.**

The arena has a dedicated slot for every field of **every** packet, which is what makes it one
allocation -- and what makes that sentence true. Two packets never share storage, so decoding a
`TC_PING` cannot disturb the `TC_SET_MODE` that arrived before it, and building a reply cannot
disturb the command being answered. One frame serves both directions of a conversation; there is no
rule here to get wrong, because there is no state to be in.

Reading is never refused on the grounds of what the frame is holding. A field nobody has written and
no frame has carried reads back as zero or its default, exactly like the section above -- one rule for values you
wrote and values that arrived, rather than one each.

**Which packet arrived is `framedsl_decode()`'s return value.** That is where the question "did this
packet come in?" is answered, once, at the point it is asked -- not later, by interrogating a field
about whether its zero means anything.

**A decode that fails changes nothing.** The walk puts the packet it is about to read aside before it
writes anything, and puts it back if it gives up part-way, so a truncated or over-long frame leaves
every field holding what it held before -- bytes and lengths both. A caller who ignores the return
value reads the previous frame's values rather than a mixture of two. Only the matched packet can be
disturbed in the first place: the match reads the buffer without writing, and check 12 guarantees
there is never a second packet to try.

**Encoding** names the packet it wants and reads only that packet's slots.

## Addressing a field

```c
bool framedsl_find(const FramedslDefinition *definition, const char *dotted, FramedslFieldId *out);
bool framedsl_field_id(const FramedslDefinition *definition, size_t packet, size_t field,
                       FramedslFieldId *out);
```

Resolve a name **once**, at startup, and use the id after that:

```c
FramedslFieldId len, payload;
framedsl_find(definition, "TM_DATAGRAM_02.len",  &len);
framedsl_find(definition, "TM_DATAGRAM_02.data", &payload);
```

`FramedslFieldId` is a small struct to be copied around, not inspected. It carries which definition it
came from, so an id resolved against one definition and used on a frame of another is
`FRAMEDSL_ERROR_FIELD` rather than a plausible wrong field -- the case that would otherwise be silent,
because two definitions of the same protocol have the same field at the same index.

`framedsl_field_id()` is the same thing for a field you already located by index, which is what you
want when walking a definition rather than looking one up.

A name with no dot in it finds nothing, as does one naming a packet or field that is not there.

## Reading and writing values

```c
FramedslStatus framedsl_set_u64(FramedslFrame *frame, FramedslFieldId id, uint64_t value);
FramedslStatus framedsl_get_u64(const FramedslFrame *frame, FramedslFieldId id, uint64_t *out);

FramedslStatus framedsl_set_bytes(FramedslFrame *frame, FramedslFieldId id,
                                  const uint8_t *bytes, size_t length);
const uint8_t *framedsl_get_bytes(const FramedslFrame *frame, FramedslFieldId id,
                                  size_t *length, FramedslStatus *status);
FramedslStatus framedsl_get_bytes_into(const FramedslFrame *frame, FramedslFieldId id,
                                       uint8_t *out, size_t capacity, size_t *length);

size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id);

FramedslStatus framedsl_set_i64(FramedslFrame *frame, FramedslFieldId id, int64_t value);
FramedslStatus framedsl_get_i64(const FramedslFrame *frame, FramedslFieldId id, int64_t *out);
FramedslStatus framedsl_set_f64(FramedslFrame *frame, FramedslFieldId id, double value);
FramedslStatus framedsl_get_f64(const FramedslFrame *frame, FramedslFieldId id, double *out);
```

**The width alone decides which pair applies.** 64 bits or fewer is a scalar and takes `u64`; anything
wider -- including every `valof()`-sized field -- is a bit string and takes `bytes`. Using the wrong
pair is `FRAMEDSL_ERROR_FIELD`, not a reinterpretation of the bytes.

**`u64` is raw bits, on every scalar, and never refused for its type.** -160 in an `i16` reads `0xFF60` and
1.5 in an `f32` reads `0x3FC00000`, so a loop copying fields keeps working, and a test can send any pattern, a
NaN included. `i64` reads and writes a signed field, and `f64` a float, an `f32` included, since a double holds
every float exactly. The suffix names the C type, as `u64` does.

On any other field the typed accessors are `FRAMEDSL_ERROR_FIELD`, and on a fixed one `FRAMEDSL_ERROR_FIXED`. A
value outside the type's range is `FRAMEDSL_ERROR_RANGE`, and the slot keeps what it had. `framedsl_set_f64()` on
an `f32` rounds to the nearest float. A finite value too large for one is `FRAMEDSL_ERROR_RANGE`, and NaN and
the infinities go through unjudged.

There is one scalar setter rather than four. A `set_u8` would add no safety that the width check does
not already give, and the compiler widens the argument at the call site for nothing.

### What gets refused

| | |
| --- | --- |
| a field with a fixed value, whatever value it is given | `FRAMEDSL_ERROR_FIXED`, **and the slot keeps what it had** |
| a value wider than the field's bits | `FRAMEDSL_ERROR_RANGE`, **and the slot keeps what it had** |
| bytes longer than the field reserved | `FRAMEDSL_ERROR_RANGE` |
| a fixed-width field given the wrong number of bytes | `FRAMEDSL_ERROR_RANGE` |
| `framedsl_get_bytes_into` with too small a buffer | `FRAMEDSL_ERROR_BUFFER`, with the size needed in `*length` |

A rejected `set_u64` changes nothing: the check runs before the write, so a caller that ignores the
status is left with the previous value rather than a truncated one.

**A fixed field cannot be set**, through `framedsl_set_u64()`, `framedsl_set_name()` or
`framedsl_set_eng()`, on a field or a member of a repeated block. The encoder writes the definition's
value whatever a program does, so a write that succeeded would be a write that never reached the wire.
Writing the value it already has is refused too, so there is one rule: a loop copying fields checks
`FramedslField.has_fixed`. A bad id, or the wrong accessor, is still `FRAMEDSL_ERROR_FIELD`, and that
is asked first; `FRAMEDSL_ERROR_FIXED` comes before any question about the value.

A **fixed-width** bit string takes exactly its own byte count -- a 72-bit field takes nine bytes and
nothing else. Accepting fewer would leave the rest of the field holding whatever was there before,
which is a wrong frame rather than a partial one. A **variable-width** field takes anything from zero
bytes up to its reservation, and remembers how much arrived; whatever sat past the new length is
cleared, so a short value never leaves a longer one's tail behind it.

### Reading a bit string without copying it

`framedsl_get_bytes()` hands back a **pointer into the frame** rather than filling a buffer, for the
same reason `framedsl_encode()` does: the bytes are already there, in their final form, and a copy
would be work done on the caller's behalf that the caller did not ask for.

```c
size_t length;
const uint8_t *data = framedsl_get_bytes(frame, data_id, &length, NULL);
if (data != NULL) {
  write(fd, data, length);
}
```

**The address does not change for the life of the frame.** The arena is allocated once and every
field has a fixed place in it, so this returns the same pointer every time. What changes is what is
behind it, and only when *that field* is written:

| | |
| --- | --- |
| `framedsl_set_bytes()` on that field | contents change |
| a **successful** `framedsl_decode()` of the packet it belongs to | contents change |
| `framedsl_frame_reset()` | contents zeroed, length back to 0 |
| a decode of **any other** packet | nothing -- separate slots |
| a **failed** decode | nothing -- the walk puts back what it disturbed |

`*length` moves independently of the address: a shorter frame changes the length and leaves the
pointer where it was. A caller holding the pointer across a decode has to read the length again
rather than assume it.

A field holding nothing returns a usable pointer and a length of `0`. `NULL` is reserved for a call
that failed -- a scalar id, or a `NULL` frame or `length` -- with the reason in `*status`, which may
itself be `NULL` if you do not want it. The slack past `*length` is always zeroed, so a caller who
ignores the length reads zeros rather than the tail of some longer frame.

`framedsl_get_bytes_into()` is the one that copies, for bytes that have to outlive the next frame or
that are going somewhere which needs to own them. It reports the size needed when the buffer is too
small, so a caller can size one from the answer instead of from a guess; passing `NULL` and `0` is
how to ask for that size.

`framedsl_byte_length()` answers the length alone, which is mostly useful for sizing a buffer before
`framedsl_get_bytes_into()`. It returns a `size_t` and so has no status to give: a field that is not
a bit string answers `0` just as an empty one does. Ask `framedsl_get_bytes()` where that difference
matters.

### `NULL`

Every function here tolerates a `NULL` frame or definition: the accessors return
`FRAMEDSL_ERROR_MEMORY`, the lookups return `false`, and `framedsl_frame_free(NULL)` and
`framedsl_frame_reset(NULL)` do nothing. A caller whose load failed is holding `NULL`, and finding
that out should not mean crashing.

## Encoding

```c
const uint8_t *framedsl_encode(FramedslFrame *frame, const char *packet,
                               size_t *length, FramedslStatus *status);
```

```c
size_t length;
FramedslStatus status;

const uint8_t *bytes = framedsl_encode(frame, "TM_DATAGRAM_02", &length, &status);
if (bytes != NULL) {
  send(socket, bytes, length, 0);
}
```

**The bytes belong to the frame.** They are valid until the next call on it, which is the shape the
usual caller wants -- encode, hand them to a socket, forget them. A caller who needs them to outlive
that copies them.

`length` is in bytes and needs no companion bit count: a packet is a whole number of bytes or it did
not load (check 13).

Nothing is allocated. The output buffer was sized when the frame was created, for the largest frame
the definition can produce.

### What the encoder decides for you

**Fixed-value fields are written from the definition, never from the arena**, and setting one is
`FRAMEDSL_ERROR_FIXED` -- a caller cannot emit a frame that fails to identify itself.

**`@bit_order` and `@byte_order` are applied as resolved**, per field. `msb_first` puts the value's
most significant bit at the earliest bit the field occupies; `lsb_first` puts its least significant
bit there. `byte_order` reorders the bytes of a field that is byte-aligned and a whole number of bytes
wide, and does nothing to any other -- which check 10 already made sure nobody wrote by mistake.

**A bit string goes out as the bytes it was given.** Neither attribute applies to a field over 64 bits
or a `valof()`-sized one; check 15 rejects writing one on such a field. These are bytes somebody
handed over, already in whatever order the thing that produced them wrote, and the library has no
basis to reorder an opaque payload. A caller who does want them reversed reverses them before calling
`framedsl_set_bytes()`.

### What it refuses

| | |
| --- | --- |
| a packet name the definition does not have | `FRAMEDSL_ERROR_FIELD` |
| a width that overflows, divides by zero, or would go below zero | `FRAMEDSL_ERROR_RANGE` |
| a resolved width past what the field reserved | `FRAMEDSL_ERROR_RANGE` |
| a length field that disagrees with the bytes set on the field it sizes | `FRAMEDSL_ERROR_RANGE` |

That last one is worth spelling out. Given

```
len:      8;
data[8]:  valof(len) * 8;
```

a caller who sets `len` to 2 and then hands `data` four bytes has described two different frames. The
definition is what decides how long `data` is, so the disagreement is an error rather than a silent
truncation -- a frame whose own length field contradicted its contents would be rejected by whatever
read it, at the far end of a link, with nothing to say why.

`status` may be `NULL` if the reason is not wanted. On any failure the call returns `NULL` and there is
nothing for the caller to read.

### Into a buffer you already have

```c
FramedslStatus framedsl_encode_into(FramedslFrame *frame, const char *packet,
                                    uint8_t *buffer, size_t capacity, size_t *length);
```

```c
uint8_t slot[64];
size_t length;

if (framedsl_encode_into(frame, "TM_DATAGRAM_02", slot, sizeof(slot), &length) == FRAMEDSL_OK) {
  send(socket, slot, length, 0);
}
```

Same encode, writing where the caller says. For somebody who has a place the frame has to end up
anyway -- a transmit ring, a region of a larger message, a buffer whose lifetime is theirs rather
than the frame's. `framedsl_encode()` stays the one to reach for otherwise; this exists because
"valid until the next call on the frame" is an answer some callers cannot use.

**`*length` always receives the size of the encoded frame**, including when that size is the reason
the call failed:

| | |
| --- | --- |
| it fits | `FRAMEDSL_OK`, `*length` is how many bytes were written |
| too small | `FRAMEDSL_ERROR_BUFFER`, `*length` is the size needed, **and `buffer` is untouched** |
| anything else | whatever `framedsl_encode()` would have said |

Untouched matters more than it looks. A partial frame left in a caller's ring is a frame that gets
sent, and the receiver at the far end has no way to know it was refused here.

A `NULL` buffer with a capacity of `0` is the useful degenerate case: it is how you ask how big the
frame would be without writing it anywhere.

Internally this builds the frame in the frame's own buffer and copies. That looks like a copy too
many until you ask what the alternative is: a packet with a `valof()` field has no length until the
walk that produces it has finished, so encoding straight into the caller's buffer means either
writing into it before knowing whether it is big enough -- which breaks the promise above -- or
walking every width twice, which costs more than the copy. Nothing is allocated either way.

## Decoding

```c
const char *framedsl_decode(FramedslFrame *frame, const uint8_t *buffer, size_t length,
                            size_t *consumed, FramedslStatus *status);
```

```c
size_t consumed;
FramedslStatus status;

const char *packet = framedsl_decode(frame, bytes, length, &consumed, &status);
if (packet != NULL && strcmp(packet, "TM_DATAGRAM_02") == 0) {
  framedsl_get_u64(frame, len_id, &value);
}
```

**You do not say which packet it is.** That is the whole reason every packet must carry a
discriminator (check 11) and no two may be confusable (check 12): the frame says what it is. Decode
matches the incoming bytes against the bits each packet pins, and at most one can match.

The name it returns points into the definition and lives as long as it does. It is the symmetric
counterpart of the name `framedsl_encode()` takes.

### A packet the caller names

```c
FramedslStatus framedsl_decode_as(FramedslFrame *frame, const char *packet, const uint8_t *buffer,
                                  size_t length, size_t *consumed);
bool framedsl_packet_manual(const FramedslDefinition *definition, size_t index);
```

A link can carry identity outside the bytes, as a CAN ID does. A `@decode: manual` packet is decoded by name,
and `framedsl_decode()` never returns one:

```c
void on_can_frame(uint32_t can_id, const uint8_t *data, size_t length) {
  const char *packet = can_id == 0x100 ? "ENGINE" : "BATTERY";
  size_t consumed;
  FramedslStatus status = framedsl_decode_as(frame, packet, data, length, &consumed);
  if (status != FRAMEDSL_OK) {
    return;
  }
  /* The fields read as usual. */
}
```

The caller has the name, so it returns a status, as `framedsl_encode_into()` does. `consumed` may be `NULL`, and
bytes past the packet are left for the caller. A name the definition lacks, or an `auto` packet, is
`FRAMEDSL_ERROR_FIELD`. Only size denies the decode: `FRAMEDSL_ERROR_TRUNCATED` when the bytes run out, and
`FRAMEDSL_ERROR_RANGE` when a length passes its reservation or goes below zero. A fixed value that differs reads
as what arrived, and a decode that fails changes nothing. Encoding a `manual` packet by name gives the bare
payload.

### `consumed`, and reading a stream

`consumed` receives how many bytes the frame occupied. Bytes after it are left alone, so a buffer
holding several frames is walked by decoding and advancing:

```c
size_t offset = 0;
while (offset < length) {
  size_t consumed;
  const char *packet = framedsl_decode(frame, buffer + offset, length - offset, &consumed, &status);
  if (packet == NULL) {
    break;
  }
  handle(packet, frame);
  offset += consumed;
}
```

This is not a convenience. A packet with a `valof()` field has no length until it has been decoded, so
without it a caller would have to re-implement that field's arithmetic by hand to find where the frame
ended — which is the thing this library exists to avoid. `consumed` may be `NULL` if the frame is
known to be alone in the buffer.

### Three failures, three meanings

| | | |
| --- | --- | --- |
| `FRAMEDSL_ERROR_UNRECOGNISED` | nothing here matches any packet | noise on the link, or a sender running a newer definition |
| `FRAMEDSL_ERROR_TRUNCATED` | a packet matched, then the bytes ran out | usually a framing bug in the caller's transport |
| `FRAMEDSL_ERROR_RANGE` | a length field claims more than its field reserved | usually a corrupt frame |

They are separate values because they need different responses. An unrecognised frame is something to
count and drop; a truncated one means the transport is handing over the wrong bytes; an over-bound
length means the frame is corrupt. A single "decode failed" would leave a program unable to tell a
noisy link from a broken one.

The last is what the mandatory reservation buys. A frame claiming more than the field reserved stops
the walk: the slot is never overrun, nothing is allocated to fit a number a corrupt frame supplied,
and the caller is told. Nothing is read past `buffer + length` in any case.

**A decode that fails changes nothing.** The packet being walked is put aside first and put back if
the walk gives up, so every field still holds what it held before -- not half of the frame that did
not arrive. A caller who ignored the return value reads the previous frame's values rather than a
mixture of two. See [what a frame holds](#what-a-frame-holds).

The same rule is what stops a decoded frame answering for a packet that did not arrive.

## Calibrations

A field with a `@cal:` reads and writes in engineering units through its curve, as well as in raw
counts exactly as before. The arena only ever holds the raw value, so a curve costs nothing until
somebody asks for the engineering one.

```c
double celsius;
framedsl_get_eng(frame, board_id, &celsius);   /* raw -> engineering */
framedsl_set_eng(frame, limit_id, 35.0);       /* engineering -> raw, rounded to the nearest count */
```

`framedsl_set_eng()` needs the curve run backwards. A linear one is inverted automatically; any other
needs an `inverse` in the definition, and without one is `FRAMEDSL_ERROR_FIELD`. `FramedslField`
says which in advance: `cal` is the curve's name or `NULL`, and `cal_invertible` whether it can be
set. A value whose raw counterpart does not fit the field -- negative on an unsigned one, or past a
signed type's range -- is `FRAMEDSL_ERROR_RANGE`, and leaves the field alone. On a typed field the curve
sees the value, -160 or 1.5, rather than the pattern: `framedsl_set_eng()` rounds to the nearest integer
on a signed field, and does not round on a float. A reading the curve cannot turn into a finite number -- a division by a
value that happened to be zero -- is `FRAMEDSL_ERROR_RANGE` too, rather than an infinity.

**What round-tripping costs.** Only the rounding: a value set and read back is within half a raw
step of the one set, which for a line `a * raw + b` is `|a| / 2`.

### Calling a curve directly

A curve can be used without a field, for a threshold or a limit a program keeps in engineering
units:

```c
FramedslCalId volts;
framedsl_find_cal(definition, "volts", &volts);

double v, raw;
framedsl_calibrate(definition, volts, 2048, &v);     /* forward */
framedsl_uncalibrate(definition, volts, 1.2, &raw);  /* backward, not rounded */
```

Like a field id, a calibration id is bound to the definition it came from.

### Compiled in: `framedsl_emit_c()`

For code that wants a curve compiled in rather than looked up, `framedsl_emit_c()` -- or
`frame-dsl defs.fdsl --emit-c` -- writes every calibration as a C header of `static inline double`
functions:

```c
static inline double fdsl_celsius(double raw) {
  return 0.0625 * raw + (-40.0);
}

static inline double fdsl_celsius_inverse(double eng) {
  return (eng - (-40.0)) / 0.0625;
}
```

The header is generated from the compiled curves, not the source, so it computes exactly what the
library does, bit for bit -- as long as the compiler keeps the arithmetic as written: no
`-ffast-math`, and no fused multiply-add contraction (`-ffp-contract=off`, already the default under
`-std=c11`). It is snprintf-style: pass `NULL` and `0` to ask its length. The prefix is yours to
choose, so two definitions' curves can live in one program.

### How fast

A curve that reduces to a line -- most do, including one that calls another line -- is compiled to
one multiply and one add, with the constants folded when the file loads. Anything else is a short
postfix program over a fixed stack, calls inlined, constants folded, and no allocation. The emitted
C is the same arithmetic without the interpreter.

## Enumerations

A field typed by an enum, `level: TxLevel;`, is a scalar like any other: the arena holds its number,
and `framedsl_get_u64()` and `framedsl_set_u64()` read and write it whatever it is. Its names are the
checked path:

```c
FramedslStatus framedsl_get_name(const FramedslFrame *frame, FramedslFieldId id, const char **name);
FramedslStatus framedsl_set_name(FramedslFrame *frame, FramedslFieldId id, const char *name);
```

```c
const char *level;
if (framedsl_get_name(frame, level_id, &level) == FRAMEDSL_OK) {
  printf("transmitting at %s\n", level);          /* "HIGH" */
}
framedsl_set_name(frame, level_id, "MAX");         /* writes 7 */
```

A decoded value the enum does not list is not a failed decode: the frame arrived, and the number is
in it. `framedsl_get_name()` says so with `FRAMEDSL_ERROR_UNLISTED` and a `NULL` name, and
`framedsl_get_u64()` still reads the number -- which is what somebody debugging a device that sends a
state nobody told the definition about wants to see. A name the enum does not list is
`FRAMEDSL_ERROR_UNLISTED` too, and writes nothing. A field that is not typed by an enum is
`FRAMEDSL_ERROR_FIELD` to both. The name is as the definition writes it, `MAX`, never `TxLevel.MAX`.

Why the raw value stays writable: an encoder that refused an unlisted number could not build the frame
that tests how a device handles one.

### Every enum and its names

```c
size_t      framedsl_enum_count(const FramedslDefinition *def);
const char *framedsl_enum_name(const FramedslDefinition *def, size_t index);
bool        framedsl_find_enum(const FramedslDefinition *def, const char *name, size_t *index);
size_t      framedsl_enum_item_count(const FramedslDefinition *def, size_t index);
bool        framedsl_enum_item(const FramedslDefinition *def, size_t index, size_t item,
                               const char **name, uint64_t *value);
```

For a program that shows a field's choices -- a menu, a command-line completion -- rather than
knowing them already. `FramedslField.enumeration` names a field's enum, and `framedsl_find_enum()`
resolves it; the names come back in the order written, which is increasing value. An imported enum
is named as the definition names it, `vendor.Mode` for one imported `as vendor`.

### In the emitted C

`framedsl_emit_c()` writes every enum after the calibrations, as constants and a function from a
value to its name:

```c
/* enum TxLevel: 3 bits */
enum fdsl_TxLevel {
  fdsl_TxLevel_OFF = 0,
  fdsl_TxLevel_LOW = 1,
  fdsl_TxLevel_MID = 2,
  fdsl_TxLevel_HIGH = 3,
  fdsl_TxLevel_MAX = 7,
};

static inline const char *fdsl_TxLevel_name(unsigned long long value) {
  switch (value) {
  case 0ULL:
    return "OFF";
  ...
  default:
    return (const char *)0;
  }
}
```

The constants are a C `enum` when every value fits an `int`, which is all an enumeration constant can
be; an enum with a larger value -- a 64-bit field's -- gets `#define`s instead. Either way each is a
constant expression, usable in a `case`.

## Conditional fields

A field under `[ @if: valof(sec_hdr) == 1; ]` is in a frame only when its condition holds -- every
condition around it, for one in a nested group. Whether it is, is worked out from the values the frame
holds now, not stored: for a decoded frame that is the fields it carried, and for one being built, the
fields `framedsl_encode()` will write.

```c
FramedslStatus framedsl_present(const FramedslFrame *frame, FramedslFieldId id, bool *present);
```

```c
bool present;
framedsl_present(frame, time_id, &present);
if (present) {
  framedsl_get_u64(frame, time_id, &seconds);
}
```

Reading a field the frame does not hold is `FRAMEDSL_ERROR_ABSENT`, from every getter --
`framedsl_get_u64()`, `framedsl_get_bytes()` and its copying twin, `framedsl_get_eng()`,
`framedsl_get_name()` and `framedsl_count()`; `framedsl_byte_length()` says 0. Whatever its slot holds
is not something the frame says, and a zero would be indistinguishable from a zero that was sent. Asking
for the wrong kind of value is still `FRAMEDSL_ERROR_FIELD` first. A member of a repetition is present
when its repetition is.

**Writing one is not refused.** A field can be set while its condition does not hold -- the value
waits in its slot, and `framedsl_encode()` leaves the field out until the condition holds. So fields can
be set in any order, the field before the flag that turns it on as well as after.

**A decode clears what it did not carry.** A decoded frame without the field leaves its slot as a frame
starts, zero or its default, and a variable one empty, so setting the flag afterwards reads a field
nobody has written rather than the one an earlier frame carried. The same goes for the elements of a
repetition past the last one a frame carried.

A condition, a width or a count reading a fixed field reads what the frame holds: its fixed value on a
frame being built, and what arrived on a decoded one. Past the fields that identify a packet those can
differ, and the frame answers from what arrived: a decoded frame's `framedsl_present()` and
`framedsl_count()` agree with the bytes the decode consumed.

The encoder always writes the definition's value, so a decoded frame encoded again may differ from what
arrived, and `framedsl_present()` then describes what the frame holds rather than what would go out.

## A whole program

```c
#include <stdio.h>
#include <string.h>
#include "framedsl.h"

int main(void) {
  FramedslResult *result = framedsl_load_file("frames.fdsl");
  if (!framedsl_ok(result)) {
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *d = framedsl_diagnostic(result, i);
      const char *where = d->file != NULL ? d->file : framedsl_source_name(result);
      fprintf(stderr, "%s:%d:%d: %s\n", where, d->line, d->column, d->message);
    }
    framedsl_result_free(result);
    return 1;
  }

  const FramedslDefinition *definition = framedsl_definition(result);
  FramedslFrame *frame = framedsl_frame_new(definition);

  /* Resolved once, at startup. */
  FramedslFieldId len, data, crc;
  framedsl_find(definition, "TM_DATAGRAM_02.len",  &len);
  framedsl_find(definition, "TM_DATAGRAM_02.data", &data);
  framedsl_find(definition, "TM_DATAGRAM_02.crc",  &crc);

  const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
  framedsl_set_u64(frame, len, sizeof(payload));
  framedsl_set_bytes(frame, data, payload, sizeof(payload));
  framedsl_set_u64(frame, crc, 0x1234);

  size_t length;
  const uint8_t *bytes = framedsl_encode(frame, "TM_DATAGRAM_02", &length, NULL);
  if (bytes != NULL) {
    send(socket, bytes, length, 0);          /* ...or whatever carries it */
  }

  /* ...and at the far end. */
  size_t consumed;
  const char *packet = framedsl_decode(frame, received, received_length, &consumed, NULL);
  if (packet != NULL) {
    uint64_t value;
    framedsl_get_u64(frame, len, &value);
  }

  framedsl_frame_free(frame);
  framedsl_result_free(result);
  return 0;
}
```

When the frame changes, `frames.fdsl` changes. This program does not get rebuilt.
