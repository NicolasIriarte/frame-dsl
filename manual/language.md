# The Frame-DSL language

What you need to write a definition file. This describes **v1**: the language as it is being
implemented first. The formal grammar is in [`docs/grammar.ebnf`](../docs/grammar.ebnf); the
reasoning behind the design is in [`docs/design.md`](../docs/design.md).

A definition file describes the layout of packets. It does not describe what they mean, when they
are sent, or how they travel -- see [`docs/scope.md`](../docs/scope.md).

## A whole file

```
// Applies to every packet below.
bit_order  msb_first;
byte_order big_endian;

pkt TM_DATAGRAM_02 {
  some_header: 8 = 0xAA;             // fixed value: identifies this packet
  len:         32;                   // counts payload bytes
  data:        valof(len) * 8        // width in bits, so convert
               max 1024;             // and bound it
  crc:         16;
};
```

## Packets

```
pkt NAME {
  ...fields...
};
```

The trailing `;` after the closing brace is required. Packet names are unique within a file, and a
packet must declare at least one field.

## Fields

A field is a name, a width **in bits**, and an optional fixed value:

```
name: <width> [max <bound>] [= <value>] [<settings>];
```

Widths are always bits. There is no byte unit anywhere in the language -- see
[Why bits everywhere](#why-bits-everywhere).

```
base:    11;          // an 11-bit field
dlc:      4;          // a 4-bit field
seq:     15;          // a 15-bit field
```

Field names are unique within their packet. Fields are laid out in declaration order, with no
implicit padding: the next field starts at the bit after the previous one ends.

### Integers accept three bases

```
flags: 8 = 0xAA;      // hex
mode:  4 = 0b1010;    // binary
count: 8 = 170;       // decimal
```

### Scalars and bit strings

A field of **64 bits or fewer** is a scalar: an unsigned integer, read and written with the scalar
API (`framedsl_set_u32`, `framedsl_get_u32`, and so on).

A field **wider than 64 bits** -- including every `valof()`-sized field -- is a bit string, read and
written as bytes with the array API (`framedsl_set_arr`, `framedsl_get_arr`).

The rule is the width alone. Nothing else has to be declared.

## Fixed values, and how packets are told apart

A field with `= <value>` always has that value: the encoder writes it whether or not you set it, and
the decoder uses it to recognise the packet.

```
some_header: 8 = 0xAA;
```

**Every packet must have at least one fixed-value field.** A packet with none cannot be recognised,
and a definition file containing one is rejected when it loads -- not when a frame arrives.

Definitions whose packets *could* be confused for one another are rejected the same way, with a
message naming the pair. Two packets are distinguishable when there is some position both constrain
to a fixed value and they disagree there:

```
pkt A { hdr: 8 = 0xAA; body: 16; };
pkt B { hdr: 8 = 0xBB; body: 16; };   // fine: differ at bits 0..7

pkt C { hdr: 8 = 0xAA; kind: 8 = 0x01; };
pkt D { hdr: 8 = 0xAA; kind: 8 = 0x02; };   // fine: differ at bits 8..15

pkt E { hdr: 8 = 0xAA; n: 16; };
pkt F { hdr: 8 = 0xAA; m: 32; };      // REJECTED: nothing separates them
```

A fixed value must fit in its field's width: `x: 4 = 0xFF;` is an error.

## Fields whose length comes from another field

`valof(name)` is the value of an earlier field in the same packet. It is the field's **raw numeric
value**, so any conversion is written out:

```
len:  32;                       // counts payload BYTES
data: valof(len) * 8 max 1024;  // ...so multiply to get bits
```

Arithmetic is `+ - * /` with the usual precedence and parentheses, e.g. when a length counts itself:

```
len:  16;                            // counts the whole packet, itself included
body: valof(len) * 8 - 16 max 4096;
```

Two rules, both checked at load time:

1. **`valof()` may only name a field declared before it**, in the same packet. A width that is not
   known until later in the frame cannot be used to read what came earlier.
2. **A width that uses `valof()` must declare `max`.** The bound is a constant -- it cannot itself
   contain `valof()`.

`max` is what makes the no-allocation guarantee true: the arena reserves the bound when the file
loads. A frame whose length field implies more than the bound is a decode error, with nothing
allocated and nothing truncated.

## Bit ordering and byte order

Two independent settings:

| Setting | Values | Default | Controls |
| --- | --- | --- | --- |
| `bit_order` | `msb_first`, `lsb_first` | `msb_first` | the order a field's bits enter the stream |
| `byte_order` | `big_endian`, `little_endian` | `big_endian` | the order the bytes of a multi-byte value appear |

Both are written the same way at three scopes, and the innermost wins -- **field > packet > file**:

```
bit_order  msb_first;                    // file: applies to everything below
byte_order big_endian;

pkt TM_LITTLE {
  byte_order little_endian;              // packet: applies to this packet

  hdr:   8 = 0xC3;
  count: 32 byte_order big_endian;       // field: applies to this field only
  value: 32;                             // little_endian, from the packet
};
```

File-level settings come before the first packet; packet-level settings come before the first field.

`byte_order` only means something for a field that is byte-aligned and a whole number of bytes wide.
Writing it on one that is not -- `seq: 15 byte_order little_endian;` -- is an error rather than a
silently ignored line.

Inheriting it is different, and is not an error. A file- or packet-level `byte_order` applies to
every field below it, and most real frames mix byte-wide fields with sub-byte ones; the setting
simply does not apply to the fields that have no bytes to reorder. Only a setting *written* where it
can do nothing is rejected, because only that means someone believed they had specified something.

## Comments

```
// to end of line

/* block, which does not nest */
```

## Complete examples

[`examples/`](../examples) holds definition files that are kept valid against this document and are
reused as the parser's test fixtures.

## Why bits everywhere

Sizes could have been declared in bytes where they happen to be whole bytes, and `valof()` could
have carried a unit. Both were rejected for the same reason: the unit would then be implied by
context, and in real protocols the context is not reliable. A length field may count payload bytes,
or the whole frame, or the frame minus its header, or 32-bit words. Since arithmetic is needed to
express those anyway, the language makes the arithmetic the normal case and keeps one unit
everywhere, so a width never has to be read twice to know what it means.

## Not in v1

Deferred, each tracked by its own issue: signed integers, floating-point and fixed-point fields,
enumerations, conditional fields, repetition, packet composition, CRC polynomial and range, and
calibration curves.

`delimiter;` from the original sketch is **not** part of the language. It had no width and no defined
meaning; it can return as a defined construct if a real frame needs one.
