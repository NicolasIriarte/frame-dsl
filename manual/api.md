# The C API

Everything a program needs is in [`include/framedsl.h`](../include/framedsl.h). It is the only header
to include, and it depends on nothing but the C standard library.

This covers **loading a definition and asking what is in it**. Encoding and decoding are not here
yet -- they need the arena and the bytecode, which are being designed against a working encoder
rather than guessed at now. See [`docs/design.md`](../docs/design.md).

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
| `FRAMEDSL_ERROR_MEMORY` | allocation failed |

`IO` and `INVALID` are separate on purpose. A file that is not there is a different problem from a
file that is wrong, and a caller should not have to read the message to tell them apart.

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
| `max_bits` | the declared bound, when it is (`0` when it is not) |
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
