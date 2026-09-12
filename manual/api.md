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
      fprintf(stderr, "%s:%d:%d: %s\n", framedsl_source_name(result), d->line, d->column, d->message);
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
void framedsl_result_free(FramedslResult *result);
```

`name` is what diagnostics are attributed to, and may be `NULL`. The caller owns the result. Freeing
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

`IO` and `INVALID` are separate on purpose. A file that is not there is a different problem from a
file that is wrong, and a caller should not have to read the message to tell them apart.

The last three belong to the frame half below. They are in the same enum rather than one of their
own, so a caller that prints a status has one function to write -- and "which call failed" never
needs asking, because it is the one that returned something other than `FRAMEDSL_OK`.

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
} FramedslDiagnostic;
```

Diagnostics come back in file order. **`check` is the field worth using in a test**: "the file was
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
| `offset_known` | false for every field after a variable-width one |
| `offset_bits` | where it starts, when that is known |
| `is_scalar` | 64 bits or fewer: read as an integer, not as bytes |
| `has_fixed`, `fixed` | the value that identifies the packet, if this field carries one |
| `bit_order`, `byte_order` | **resolved** |

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
reads back as zero and will encode as zero. `framedsl_frame_reset()` puts it all back to that state.

This is a deliberate choice rather than an omission. Tracking which fields a caller had set would
mean a bit per field, a rule about what `reset` does to it, and an encode that fails on a field the
caller quite reasonably wanted to leave at zero.

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
FramedslStatus framedsl_get_bytes(const FramedslFrame *frame, FramedslFieldId id,
                                  uint8_t *out, size_t capacity, size_t *length);

size_t framedsl_byte_length(const FramedslFrame *frame, FramedslFieldId id);
```

**The width alone decides which pair applies.** 64 bits or fewer is a scalar and takes `u64`; anything
wider -- including every `valof()`-sized field -- is a bit string and takes `bytes`. Using the wrong
pair is `FRAMEDSL_ERROR_FIELD`, not a reinterpretation of the bytes.

There is one scalar setter rather than four. A `set_u8` would add no safety that the width check does
not already give, and the compiler widens the argument at the call site for nothing.

### What gets refused

| | |
| --- | --- |
| a value wider than the field's bits | `FRAMEDSL_ERROR_RANGE`, **and the slot keeps what it had** |
| bytes longer than the field reserved | `FRAMEDSL_ERROR_RANGE` |
| a fixed-width field given the wrong number of bytes | `FRAMEDSL_ERROR_RANGE` |
| `framedsl_get_bytes` into too small a buffer | `FRAMEDSL_ERROR_BUFFER`, with the size needed in `*length` |

A rejected `set_u64` changes nothing: the check runs before the write, so a caller that ignores the
status is left with the previous value rather than a truncated one.

A **fixed-width** bit string takes exactly its own byte count -- a 72-bit field takes nine bytes and
nothing else. Accepting fewer would leave the rest of the field holding whatever was there before,
which is a wrong frame rather than a partial one. A **variable-width** field takes anything from zero
bytes up to its reservation, and remembers how much arrived; whatever sat past the new length is
cleared, so a short value never leaves a longer one's tail behind it.

`framedsl_get_bytes()` reports the size needed when the buffer is too small, so a caller can size one
from the answer instead of from a guess. `framedsl_byte_length()` asks the same question without
copying anything.

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

**Fixed-value fields are written from the definition, never from the arena.** Setting `hdr` to
something other than what the definition pins it to does not change the frame that comes out -- a
caller cannot emit one that fails to identify itself.

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

A decode that fails leaves the arena holding whatever the walk had reached. Do not read it.

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
      fprintf(stderr, "%s:%d:%d: %s\n", framedsl_source_name(result), d->line, d->column, d->message);
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

  /* ...and at the far end, over the same definition file. */
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
