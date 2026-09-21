# The Frame-DSL language

What you need to write a definition file. This describes **v1**, the language as it is being
implemented first, plus calibrations. The formal grammar is in [`docs/grammar.ebnf`](../docs/grammar.ebnf); the
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
`@name: value;`. There are three:

| Attribute | Values | Default | Controls |
| --- | --- | --- | --- |
| `@bit_order` | `msb_first`, `lsb_first` | `msb_first` | the order a field's bits enter the stream |
| `@byte_order` | `big_endian`, `little_endian` | `big_endian` | the order the bytes of a multi-byte value appear |
| `@cal` | the name of a `cal` | none | the curve between a field's raw and engineering value -- see [Calibrations](#calibrations) |

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

An anonymous block inside a packet has no name. A block with a name is a different thing, declared
at file level to be reused -- see [Reusing a group of fields](#reusing-a-group-of-fields).

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

## Reusing a group of fields

Most protocols give every packet the same header, and change one field of it -- the id that says
which packet this is. A **named block** is written once at file level and used as a field whose width
is the block's name:

```
block Header {
  id:       8;              # left open: every packet fixes its own
  sequence: 16;
  version:  4 = 0x1;
  flags:    4;
};

block Vector3 { x: 16; y: 16; z: 16; };

pkt TC_SET_LIMIT {
  hdr:     Header { id = 0x02; };
  channel: 8;
  limit:   32;
};

pkt TM_IMU {
  hdr:   Header { id = 0x83; flags = 0x3; };
  accel: Vector3;
  gyro:  Vector3;
};
```

**Names.** A use is an *instance* with a name of its own, and the block's fields appear under it:
`TC_SET_LIMIT.hdr.sequence`, `TM_IMU.gyro.z`. That is the name the C API's `framedsl_find()`, `-e`
and `-d` all use. It is also why the same block can appear twice in one packet.

**Overrides.** `{ id = 0x02; }` fixes a field for this use only -- which is how one shared header
becomes each packet's discriminator. It may replace a value the block fixed, and reaches into a
nested instance by its path, `{ inner.id = 3; }`. An override fixes a value and nothing else: a
block's widths and field order are the same in every packet that uses it.

**Layout is decided where the block is declared.** Its attributes -- bit order, byte order, `@cal` on
its fields -- come from the file default and from what is written inside it, not from the packet it
is used in. A `[ ... ]` group in front of a use is an error for that reason; put the attribute in the
block.

**Blocks nest.** A block may use another, and a `valof()` inside a block sizes a field from the same
instance:

```
block Sample {
  axes:      Vector3;
  size:      8;
  extra[32]: valof(size) * 8;       # this instance's size, whatever else is called `size`
};
```

**Declared before use.** A block must appear above anything that uses it -- a packet or another
block. That is also what rules out a block containing itself: inside its own braces it has not been
declared yet.

A packet built from blocks is exactly the packet you would get by writing their fields out, and costs
nothing more at run time: blocks are expanded when the file loads, and nothing after that knows they
existed. Checks 23 to 25 in [`docs/design.md`](../docs/design.md) cover an unknown block, one used
before it is declared, and an override naming no field of its block.

## Calibrations

A field's raw value is often not the number anybody wants. A 12-bit ADC reading is counts; what the
operator wants is volts. A calibration is a named function from one to the other, declared at file
level with `cal` and attached to a field with `@cal:`:

```
cal volts(raw)   = raw * 3.3 / 4095;
cal current(raw) = volts(raw) / 0.05;              # a calibration may call another
cal ohms(raw)    = 10000 * raw / (4095 - raw) inverse (eng) = 4095 * eng / (10000 + eng);

pkt TM_POWER {
  id:     8 = 0x10;
  [ @cal: volts; ]
  bus:    12;
  [ @cal: ohms; ]
  sensor: 16;
};
```

The body is arithmetic on doubles: `+ - * /`, parentheses, unary minus, numbers -- whole, fractional
(`0.0625`) or with an exponent (`4e-3`) -- the parameter, and calls to other calibrations. Nothing
else. A calibration sees only the value it is given: fields are not in scope, since a curve belongs to
no packet and the same one may serve several.

**Where it goes.** `@cal:` belongs to a single field, in the `[ ... ]` group in front of it. It is
not inherited: written at file, packet or block scope it is an error rather than a default, because
a curve describes one sensor reading and handing it to every field below would be a guess about the
others.

**Encoding needs an inverse.** Reading a field through its curve always works. Setting one from an
engineering value needs the curve run backwards:

- A curve that is a straight line -- `a * raw + b`, however it is written, through however many calls
  -- is inverted automatically.
- Anything else needs an `inverse (param) = ...` clause after the body, as `ohms` has above. Without
  one the field can still be read in engineering units, and written in raw ones.

The raw value an inverse produces is rounded to the nearest integer, so a value read back is within
half a raw step of the one written.

`cal` and `inverse` are words only where a calibration is declared, so neither is reserved: a field
called `cal` is fine.

Six checks apply, 17 to 22 in [`docs/design.md`](../docs/design.md): calibration names are unique,
every calibration named must exist, a bare name must be the parameter, no calibration may call
itself, `@cal:` may only go on a scalar field, and no calibration may divide by a constant zero.

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
enumerations, conditional fields, repetition, and CRC polynomial and range.
Calibrations have no lookup tables, domain checks or built-in functions yet.

`delimiter;` from the original sketch is **not** part of the language. It had no width and no defined
meaning; it can return as a defined construct if a real frame needs one.
