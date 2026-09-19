# The Frame-DSL language

What you need to write a definition file. This describes **v1**: the language as it is being
implemented first. The formal grammar is in [`docs/grammar.ebnf`](../docs/grammar.ebnf); the
reasoning behind the design is in [`docs/design.md`](../docs/design.md).

A definition file describes the layout of packets. It does not describe what they mean, when they
are sent, or how they travel -- see [`docs/scope.md`](../docs/scope.md).

## A whole file

```
# Applies to every packet below.
@bit_order:  msb_first;
@byte_order: big_endian;

pkt TM_DATAGRAM_02 {
  some_header: 8 = 0xAA;        # fixed value: identifies this packet
  len:         32;              # counts payload bytes
  data[128]:   valof(len) * 8;  # 128 bytes of arena; width in bits, per frame
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
name[<reservation>]: <width> [= <value>];
```

Widths are always bits. The bracketed reservation is the one number in the language that is not --
see [Reserving a slot](#reserving-a-slot) and [Why bits everywhere](#why-bits-everywhere).

```
base:    11;          # an 11-bit field
dlc:      4;          # a 4-bit field
seq:     15;          # a 15-bit field
```

Field names are unique within their packet. Fields are laid out in declaration order, with no
implicit padding: the next field starts at the bit after the previous one ends.

### A packet must be a whole number of bytes

Individual fields are any width you like -- 11 bits, 4 bits, 1 bit, straddling byte boundaries as
the frame requires. What a *packet* must do is add up to a multiple of 8 bits.

```
pkt TM_SPACE_PACKET {
  version:      3 = 0b000;
  type:         1 = 0b0;
  sec_hdr_flag: 1;
  apid:        11;
  seq_flags:    2;
  seq_count:   14;
  data_length: 16;      # 48 bits: six bytes
};
```

A frame leaves the library as bytes. A packet that stopped mid-byte would have a last byte that is
part frame and part nothing, and you would have to be told how many of its bits were real in order to
send it. So the definition is rejected when it loads instead, reporting the total:

```
frames.fdsl:7:1: 'TM_ODD' is 41 bits, which is not a whole number of bytes; a packet must be a
multiple of 8 bits so that a frame of it has an unambiguous byte form
```

Nothing is padded for you. If a frame really does have unused bits at its end, declare them -- a
`reserved: 7;` field says what is there, where an invisible pad would not.

A `valof()`-sized field has to be a multiple of 8 bits too, and it has to be *visibly* so. The
library works out each width modulo 8 without knowing any frame's contents: a product is a multiple
of eight as soon as one of its factors is, whatever the other one turns out to be.

```
user_data[65536]: (valof(data_length) + 1) * 8;   # fine: one factor is 8
payload[256]:     valof(count) * 8;               # fine
spaced[256]:      valof(count) * 16 - 32;         # fine: both terms are byte multiples
odd[384]:         valof(count) * 3;               # rejected
```

What it looks for is a factor that is itself a multiple of eight. `valof(n) * 2 * 4` is rejected even
though it is always a whole number of bytes, because neither `2` nor `4` is one on its own. Write the
multiple of eight where it can be seen.

### Integers accept three bases

```
flags: 8 = 0xAA;      # hex
mode:  4 = 0b1010;    # binary
count: 8 = 170;       # decimal
```

### Scalars and bit strings

A field of **64 bits or fewer** is a scalar: an unsigned integer, read and written with
`framedsl_set_u64()` and `framedsl_get_u64()`.

A field **wider than 64 bits** -- including every `valof()`-sized field -- is a bit string, read and
written as bytes with `framedsl_set_bytes()`, and read with `framedsl_get_bytes()` -- which hands
back a pointer into the frame rather than copying.

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
pkt B { hdr: 8 = 0xBB; body: 16; };        # fine: differ at bits 0..7

pkt C { hdr: 8 = 0xAA; kind: 8 = 0x01; };
pkt D { hdr: 8 = 0xAA; kind: 8 = 0x02; };  # fine: differ at bits 8..15

pkt E { hdr: 8 = 0xAA; n: 16; };
pkt F { hdr: 8 = 0xAA; m: 32; };           # REJECTED: nothing separates them
```

A fixed value must fit in its field's width: `x: 4 = 0xFF;` is an error.

## Fields whose length comes from another field

`valof(name)` is the value of an earlier field in the same packet. It is the field's **raw numeric
value**, so any conversion is written out:

```
len:       32;                 # counts payload BYTES
data[128]: valof(len) * 8;     # ...so multiply to get bits
```

Arithmetic is `+ - * /` with the usual precedence and parentheses, e.g. when a length counts itself:

```
len:       16;                 # counts the whole packet, itself included
body[512]: valof(len) * 8 - 16;
```

Two rules, both checked at load time:

1. **`valof()` may only name a field declared before it**, in the same packet. A width that is not
   known until later in the frame cannot be used to read what came earlier.
2. **A width that uses `valof()` must reserve a slot.** See below.

## Reserving a slot

```
data[128]: valof(len) * 8;
```

The brackets say how much room this field gets in the arena: **128 bytes**, set aside once when the
file loads and never resized. It is what makes the no-allocation guarantee true. A frame whose length
field implies more than the reservation is a decode error, with nothing allocated and nothing
truncated.

It binds to the **name**, not to the width, because the two say different things. `valof(len) * 8` is
how long the field is in a particular frame; `[128]` is how much space it is given in every frame.
Writing them in one clause made a field look like it had a single, slightly confused, size.

Three rules:

- **The reservation is in bytes.** It is the only number in the language that is not bits, because it
  measures storage rather than a position in a frame. A slot is a run of memory and is always a whole
  number of bytes; a field's width is a count of bits and frequently is not.
- **It must be a constant.** It cannot contain `valof()` -- the arena is sized before any frame
  arrives, so a reservation that depended on a frame would have nothing to be computed from.
- **Only a variable-width field may have one.** `body[4]: 32;` is rejected: a 32-bit field's slot is
  already known, and a second number could only agree with the width or contradict it.

## Attributes

Anything that modifies a declaration rather than describing it is an attribute, written
`@name: value;`. There are two in v1:

| Attribute | Values | Default | Controls |
| --- | --- | --- | --- |
| `@bit_order` | `msb_first`, `lsb_first` | `msb_first` | the order a field's bits enter the stream |
| `@byte_order` | `big_endian`, `little_endian` | `big_endian` | the order the bytes of a multi-byte value appear |

Attribute names are **not** reserved words -- they mean something only after `@`. A field may be
called `bit_order` if that is what the specification calls it.

### Where an attribute goes

Two placements, one job each:

**A bare `@name: value;`** may appear only before the first packet. It is the file default.

**A `[ ... ]` group** attaches to the declaration that follows it -- a packet, a block or a field --
and applies to that declaration only.

```
@bit_order:  msb_first;              # the file default
@byte_order: big_endian;

[ @byte_order: little_endian; ]      # the whole packet
pkt TM_LITTLE {
  hdr: 8 = 0xC3;

  [ @byte_order: big_endian; ]       # this field, and not the next one
  count: 32;

  value: 32;                         # little_endian again, from the packet
};
```

The innermost one wins: **field > block > packet > file**.

A bare `@` anywhere other than the top of the file is an error. It would otherwise have to mean
either "from here on" or "the next declaration only", and one spelling meaning both depending on how
deeply it is nested is how a reader stops being able to tell which they are looking at.

### Blocks

A group applies to one declaration -- but the case that actually comes up is a *run* of fields, most
often a payload produced by a subsystem that does not share the frame's byte order. `block` is a
declaration you can attach a group to:

```
pkt TM_SUBSYSTEM {
  hdr: 8 = 0xAA;
  seq: 16;

  [ @byte_order: little_endian; ]
  block {
    voltage: 32;
    current: 32;
    temp:    16;
  };

  crc: 16;                           # big_endian again
};
```

A block does nothing except scope attributes. Its fields sit at consecutive bit offsets, continuous
with whatever is around them, and keep the same names they would have had without it -- `voltage`,
not `block.voltage`. Removing the block and writing the attribute on each of its three fields
produces an identical layout.

Blocks nest, and the innermost wins.

### Where `byte_order` does not apply

`byte_order` only means something for a field that is byte-aligned and a whole number of bytes wide.
Writing it on one that is not is an error rather than a silently ignored line:

```
pkt TM_ODD {
  hdr: 8 = 0xAA;

  [ @byte_order: little_endian; ]    # REJECTED: 15 bits, starting at bit 8
  seq: 15;

  tail: 1;
};
```

Inheriting it is different, and is not an error. A file, packet or block `byte_order` covers every
field below it, and most real frames mix byte-wide fields with sub-byte ones; it simply does not
apply to the ones with no bytes to reorder. Only an attribute *written on the field itself* is
rejected, because only that means someone believed they had specified something.

## Comments

```
# to end of line
```

That is the only comment form. There is no block comment: a definition file is short, every editor
comments a region by prefixing its lines, and it leaves `/` meaning division and nothing else.

## Complete examples

[`examples/`](../examples) holds definition files that are kept valid against this document and are
reused as the parser's test fixtures.

## Why bits everywhere

Widths could have been declared in bytes where they happen to be whole bytes, and `valof()` could
have carried a unit. Both were rejected for the same reason: the unit would then be implied by
context, and in real protocols the context is not reliable. A length field may count payload bytes,
or the whole frame, or the frame minus its header, or 32-bit words. Since arithmetic is needed to
express those anyway, the language makes the arithmetic the normal case and keeps one unit
everywhere, so a width never has to be read twice to know what it means.

The reservation is the one exception, and it is a deliberate one: it does not describe the frame at
all. It describes a slot in memory, which is a whole number of bytes by construction and has no
meaningful sub-byte size. Writing `data[128]` rather than `data[1024]` also puts it in the same unit
as the length field it usually mirrors, which is where the arithmetic mistakes actually happen.

## Not in v1

Deferred, each tracked by its own issue: signed integers, floating-point and fixed-point fields,
enumerations, conditional fields, repetition, packet composition, CRC polynomial and range, and
calibration curves.

`delimiter;` from the original sketch is **not** part of the language. It had no width and no defined
meaning; it can return as a defined construct if a real frame needs one.
