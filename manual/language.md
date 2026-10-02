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

**Every packet must have at least one fixed-value field**, somewhere a decoder knows to look: before
any field whose width depends on another, and outside any [conditional](#conditional-fields) part. A
packet with none cannot be recognised, and a definition file containing one is rejected when it loads
-- not when a frame arrives.

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

A fixed value must fit in its field's width: `x: 4 = 0xFF;` is an error. On a field typed by an enum,
a fixed value is one of the enum's names -- see [Enumerations](#enumerations).

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

A field of a block's instance is named through it, as `framedsl_find()` names it:

```
hdr:       Header;
data[128]: valof(hdr.len) * 8;
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
`@name: value;`. There are four:

| Attribute | Values | Default | Controls |
| --- | --- | --- | --- |
| `@bit_order` | `msb_first`, `lsb_first` | `msb_first` | the order a field's bits enter the stream |
| `@byte_order` | `big_endian`, `little_endian` | `big_endian` | the order the bytes of a multi-byte value appear |
| `@cal` | the name of a `cal` | none | the curve between a field's raw and engineering value -- see [Calibrations](#calibrations) |
| `@if` | `valof(name) == value` or `!=` | none | whether what follows is in a frame at all -- see [Conditional fields](#conditional-fields) |

Attribute names are **not** reserved words -- they mean something only after `@`. A field may be
called `bit_order` if that is what the specification calls it.

### Where an attribute goes

Two placements, one job each:

**A bare `@name: value;`** may appear only at the top of the file: after any imports, and before the
first declaration of any kind -- `pkt`, `block`, `cal` or `enum`. It is the file default.

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

### An attribute that changes nothing

Writing an attribute that an enclosing scope already set -- a field's `[ @byte_order: little_endian; ]`
inside a packet that already says `little_endian` -- is a **warning** (check 31): the file loads, but
the attribute says nothing, and usually means the author thought the packet said something else.
Writing the language's own default at the top of a file is not a repeat of anything written, and is
silent.

Writing the same attribute twice in *one* place -- one `[ ... ]` group, or the bare defaults at the top
of the file -- is a warning too (check 34), whether the two values agree or not:

```
[ @byte_order: little_endian; @cal: raw_counts; @byte_order: big_endian; @cal: volts; ]
value: 16;                           # big_endian, through volts: the later of each
```

The later one wins, as it always has, so the file means what it did before the warning existed; but
one of the two is a mistake, and which one the author meant is exactly what the file no longer says.

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
`TC_SET_LIMIT.hdr.sequence`, `TM_IMU.gyro.z`. That is the name the C API's `framedsl_find()` takes
and `-d` prints. It is also why the same block can appear twice in one packet.

**Pasting a block in.** Written without an instance name, a use pastes the block's fields in under
their own names, as if they had been written out where the use is:

```
pkt TM_ACK1 {
  header: Header { id = 0x81; };    # TM_ACK1.header.sequence
  result: 8;
};

pkt TM_ACK2 {
  Header { id = 0x82; };            # TM_ACK2.sequence
  result: 8;
};

pkt TM_TIMED {
  Header { id = 0x83; };
  Timestamp;                        # no overrides: no braces needed either
};
```

Each form has its own names and only those: `TM_ACK1.sequence` and `TM_ACK2.header.sequence` find
nothing. Use a name when a block appears more than once in a packet, or when its fields would clash
with the packet's own; paste it when it is a shared header and the prefix would only be noise. A
pasted field may not share a name with another field of the packet (check 2) -- the error is reported
at the use, which is the line to change. A block may be pasted into another block's declaration too,
and then its fields are that block's own fields wherever it is used.

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

## Repeating a block

A list of readings, a table of channels: a block laid out several times in a row is a **repetition**,
written with the count in brackets after the block's name:

```
block Sample {
  marker:  4 = 0xA;
  channel: 4;
  reading: 16;
};

pkt TM_SAMPLES {
  id:          8 = 0x30;
  count:       8;
  samples[48]: Sample[valof(count)];   # as many as `count` says, at most 16
  points:      Sample[2];              # always two
};
```

**The count** is a constant, or an expression with a `valof()` of a field above it, as a width can be.
Counted by a field, the repetition reserves its room like any field sized by another -- in bytes,
`samples[48]` -- and a frame whose count would need more is refused when it is decoded, and when it
is encoded. A constant count needs no reservation, and may not have one (check 14).

**Names.** An element's fields are reached through the repetition, with the element's index from
zero: `TM_SAMPLES.samples[2].reading`. That is what `framedsl_find()` takes, and what `-d`
prints. In C, a loop finds the field once without an index and picks each element with
`framedsl_element()`; `framedsl_count()` says how many elements a frame holds. `TM_SAMPLES.samples` on
its own is the whole run, read as bytes.

**Every element is the same.** A repeated block must be one size every time -- none of its widths may
come from a `valof()` -- and a whole number of bytes, so that element *i* is always *i* times that size
in, and any number of them still ends on a byte (check 32). Its fixed values are on every element on
the wire, whatever the frame's storage holds; its bit order, byte order and calibrations are the
block's, per element. A repetition takes no overrides, since there is no one instance to fix a field of
(check 33): fix a value every element carries in the block itself. Those fixed values do not
identify the packet, though: the discriminator (check 11) must be a fixed field outside any
repetition.

A repetition may sit in a block like any use, and then counts by that instance's field --
`hdr.count` for `hdr: Body;`. A repetition inside a *repeated* block is laid out correctly but not
reachable by name: its elements are bytes of the outer element.

## Importing from other files

Blocks, calibrations and enums can be declared in one file and used from others. A library of the pieces a
bus or a vendor shares is written once:

```
# common/link.fdsl
@byte_order: big_endian;

block Header  { id: 8; sequence: 16; };
block Trailer { crc: 16; };
cal volts(raw) = raw * 3.3 / 4095;
```

and imported by every file that needs it:

```
import "common/link.fdsl";                   # Header, Trailer, volts
import "common/sensors.fdsl" as sensors;     # sensors.Header, sensors.celsius

@byte_order: little_endian;

pkt TM_TEMPERATURE {
  hdr:     Header { id = 0x82; };
  reading: sensors.Header { kind = 0x1; };
  [ @cal: sensors.celsius; ] board: 16;
  end:     Trailer;
};
```

- **Flat or named.** `import "path";` brings the file's names in as they are. `import "path" as name;`
  brings them in as `name.X` instead, and only that way -- the form for two libraries that both
  declare a `Header`. Two *different* declarations of one name reaching a file flat is an error.
- **Blocks, calibrations and enums only.** An imported file's packets are checked but not imported, so
  importing a file never changes which packets a definition decodes. A file can be a library and a
  protocol at once. An imported enum is a type like a local one, `mode: vendor.Mode = TX;`: the type is
  qualified, its names never are.
- **Paths are relative to the importing file**, so a tree of definition files can be moved as a
  whole. A definition loaded from a string with `framedsl_load_string()` resolves them from the
  working directory.
- **At the top.** Imports come before anything else, even the file's own `@` defaults, and what they
  bring counts as declared there.
- **An imported block keeps its layout.** Its byte and bit order come from the file it was declared
  in, not the one importing it -- above, `hdr` is big-endian inside a little-endian packet.
- **Checked on its own.** An imported file is loaded and checked by itself first, and a problem in it
  is reported with *its* name and position. A file imported twice through different paths -- two
  libraries sharing a third -- is the same declarations reached twice, not a clash. An import cycle is
  an error.

`import` and `as` mean something only where an import is written, so neither is reserved. Checks 26
to 28 in [`docs/design.md`](../docs/design.md) cover a file that cannot be read, a cycle, and a clash.

### Declarations nothing uses

A block, calibration, enum or import that nothing in the file uses is a **warning** (check 30) -- in a
file with at least one packet. A file with no packets is a library, and everything in it is there for
somebody else to use. Something used only by an unused block does not warn: the unused block does,
and removing it is what brings the next one up.

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

Any scalar field may carry one, a fixed-value field included: nothing is set through the curve there,
since the value is fixed, but the field reads through it like any other. And a curve does not care how
its field is laid out -- `@bit_order` and `@byte_order` apply where the frame meets the wire, the curve
to the raw value once it is off it -- so the three may share a group in any order, or the orders may
come from further out:

```
[
  @cal:        volts;
  @bit_order:  lsb_first;
  @byte_order: little_endian;
]
bus: 16;
```

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

## Enumerations

A field like `level: 3;` carries values that mean something -- off, low, high -- and without a name
for each, the meaning lives in a comment. An **enum** names them, once, at file level, and a field is
typed by it:

```
enum TxLevel: 3 {
  OFF;   # 0
  LOW;   # 1
  MID;   # 2
  HIGH;  # 3
  MAX = 7;
};

enum PktType: 4 {
  HK = 0x1;
  EVENT;  # 0x2
};

block Header {
  type: PktType;
  seq:  12;
};

pkt TM_ANTENNA {
  hdr:   Header { type = HK; };
  level: TxLevel;
  pad:   5;
};

pkt TM_EVENT {
  hdr:  Header { type = EVENT; };
  code: PktType = EVENT;
  pad:  4;
};
```

**The width is the enum's.** `enum TxLevel: 3` says every field of this type is 3 bits, and a field
typed by it, `level: TxLevel;`, is written like a use of a block -- a name where a width goes. It is a
constant, from 1 to 64 bits (check 35), since such a field is a number.

**A name without a value counts on.** It is one more than the name before it, and the first is 0.
Values must increase (check 37) -- so the list reads in order, and no value can have two names -- and
each must fit the width, written or counted (check 38). Names are unique within one enum (check 36);
two enums may both have an `OFF`.

**A value is a name.** A fixed value or an override on a field typed by an enum is one of that enum's
names, written bare -- `code: PktType = EVENT;`, `Header { type = HK; }` -- since the field's type
already says which enum it is from. A number there is refused, and so is another enum's name (check 39);
so is a name on a field that is not typed by an enum. That is what makes a discriminator read as what it
means, and the checks that tell packets apart see the number behind it like any other fixed value.

**An enum is names and a width, not layout.** A field typed by one takes its bit and byte order from
where it is used, as any field does, and a `[ ... ]` group in front of it applies:

```
enum Mode: 16 { IDLE = 0x0100; RUN; };

[ @byte_order: little_endian; ]
pkt TM_MODE {
  id:   8 = 0x40;
  mode: Mode;                               # little-endian, from the packet
};
```

What else a field typed by an enum is not: it has no calibration (check 40) -- one would make it a number
in engineering units, when it reads as a name; and it is a named field, with nothing inside to paste in,
fix with `{ ... }` or repeat (check 41). An enum is a field's type as a block is, so the two share one
set of names (check 1), and an enum is declared above everything that uses it (check 24). `enum` is a
word only where a declaration starts, so a field may still be called `enum`.

**A value the enum does not list** still decodes. A device reporting a state the definition has not
heard of is what somebody debugging it most wants to see, so it is flagged rather than refused:

```
$ frame-dsl radio.fdsl -d 1000a0
# Decoded 3 bytes
pkt TM_ANTENNA {
    hdr.type       1 (0x1) = HK [PktType]
    hdr.seq        0 (0)
    level          5 (0x5) = <unlisted> [TxLevel]
    pad            0 (0)
}
```

The raw value is the field's value in the frame, as always: `framedsl_get_u64()` and
`framedsl_set_u64()` read and write it whatever it is, and `framedsl_get_name()` and
`framedsl_set_name()` are the checked path, by name -- see [the API](api.md#enumerations).

## Conditional fields

Some of a frame is there only sometimes: a secondary header a flag announces, a payload only one kind of
packet carries. `@if` in a group in front of a field, a block's use or an anonymous block makes it
**conditional** -- on the wire only when an earlier field has a value:

```
enum Kind: 4 {
  IDLE;
  HOUSEKEEPING;
  EVENT;
};

block Header {
  id:      8;
  sec_hdr: 1;
  has_crc: 1;
  spare:   2;
  kind:    Kind;
};

block Time {
  seconds: 32;
  subsecs: 16;
};

block Reading {
  channel: 8;
  value:   16;
};

pkt TM_OPTIONAL {
  hdr:  Header { id = 0x50; };
  [ @if: valof(hdr.sec_hdr) == 1; ]
  time: Time;  # TM_OPTIONAL.time.seconds, present only with a secondary header
  [ @if: valof(hdr.kind) != IDLE; ]
  block {
    count:        8;
    readings[32]: Reading[valof(count)];
    [ @if: valof(hdr.kind) == EVENT; ]
    code:         16;  # present only when kind is EVENT: both conditions hold
  };
  [ @if: valof(hdr.has_crc) == 1; ]
  crc: 16;
};
```

That is [`examples/optional_fields.fdsl`](../examples/optional_fields.fdsl).

**A condition is one comparison.** `valof(name) == value`, or `!=` for its opposite -- which is also
how an "otherwise" is written: two groups, `==` and `!=`. The field compared is one declared above,
in the same packet, and a scalar (checks 7 and 16), named through an instance where it is in one,
`valof(hdr.sec_hdr)`. The value is a number that fits it (check 8), or -- when it is typed by an enum
-- one of the enum's names, as a fixed value would be (check 39). Combining comparisons in one
condition is #126.

**Groups nest**, and what is inside needs every condition around it. One group takes one `@if`; two
would leave a reader wondering which wins, so it is an error, and nesting is how both are asked for.
`@if` shares its group with any other attribute, in any order. It goes in front of what may be absent
-- a field, a use, an anonymous block -- and not in front of a packet, a named block's declaration or
at the top of a file.

**A block's use may be conditional**, though no other attribute goes in front of one: whether a
block is there belongs to where it is used, not to its layout. Inside a block's declaration, a
condition reads the block's own fields, as a `valof()` width does. A whole repetition may be
conditional; a field of a repeated block may not, since every element must be one size (check 32).

**Whole bytes, with it or without it.** A packet is whole bytes in every frame, so each conditional
part must be whole bytes on its own (check 13) -- sub-byte fields inside one are fine as long as they
add up:

```
[ @if: valof(f) == 1; ]
block { a: 3; b: 5; };              # fine: 8 bits
[ @if: valof(f) == 2; ]
c: 4;                               # rejected: the packet would be 4 bits off when it is there
```

**Reading what may not be there.** A `valof()` -- in a width, a count or a condition -- may name a
conditional field only from under the same conditions, or stricter ones (check 42). Elsewhere it would
read a field the frame may not carry:

```
[ @if: valof(has_len) == 1; ]
block {
  len:      8;
  data[16]: valof(len) * 8;         # fine: both there, or neither
};
```

**Identifying a packet.** Nothing at or after a conditional field sits at a known offset, as after a
`valof()`-sized one, so no fixed value there identifies the packet: a fixed value inside a conditional
part is written when the part is there, and that is all. A packet whose only fixed value is in or after
one is rejected when it loads (check 11).

**Reading and writing one.** Whether a field is there is worked out from the frame's values as they are
now -- a decoded frame's, or the ones being set -- so `framedsl_present()` answers for both, and reading
a field that is not there is `FRAMEDSL_ERROR_ABSENT` rather than a value nobody sent. Setting one is
never refused: the value waits until its condition holds, and the encoder leaves it out until then.
`-d` shows what a frame did not carry:

```
$ frame-dsl examples/optional_fields.fdsl -d 50 41 00 12 34
# Decoded 5 bytes
pkt TM_OPTIONAL {
    hdr.id         80 (0x50)
    hdr.sec_hdr    0 (0)
    hdr.has_crc    1 (0x1)
    hdr.spare      0 (0)
    hdr.kind       1 (0x1) = HOUSEKEEPING [Kind]
    time.seconds   <absent>
    time.subsecs   <absent>
    count          0 (0)
    readings       0 elements
    code           <absent>
    crc            4660 (0x1234)
}
```

`if` is a word only after `@`, like every attribute's name, so a field may still be called `if`.

## Comments

```
# to end of line
```

That is the only comment form. There is no block comment: a definition file is short, every editor
comments a region by prefixing its lines, and it leaves `/` meaning division and nothing else.

## Layout

There is one layout, and `frame-dsl <file> --format` writes it -- nobody has to remember it, and a
review never has to discuss it. It is the layout every file under `examples/` is in:

```
@bit_order:  msb_first;
@byte_order: little_endian;

[ @byte_order: big_endian; ]
pkt TM_STATUS {
  id:       8 = 0x10;  # the discriminator
  hdr:      Header { flags = 0x3; };
  size:     8;
  data[64]: valof(size) * 8;
  [
    @bit_order:  lsb_first;
    @byte_order: little_endian;
  ]
  block {
    a:  8;
    bb: 8;
  };
  crc: 16;
};
```

- **Two spaces a level**, and no tabs anywhere. Packets, blocks and multi-line attribute groups nest.
- **Fields align on their widths.** The colon stays on the name, and the width starts in one column
  for every field of a *run*: the fields directly inside one packet or block. Blank lines, comments
  and attribute groups do not end a run; a nested `block { }` does, so the fields after it align on
  their own. File settings align on their values the same way, and so do the settings of a group.
- **Trailing comments align too**, two spaces past the longest line of the run that has one.
- **One setting in a group stays on one line**, `[ @byte_order: big_endian; ]`; more than one is one
  setting per line. A condition is spaced as a width is, `[ @if: valof(hdr.flag) == 1; ]`.
- **Spacing**: one space either side of `=` and of every arithmetic operator, none inside
  parentheses, none between `valof` or a calibration's name and its parenthesis. A use's overrides go
  on its line: `hdr: Header { id = 0x02; flags = 0x3; };`. A calibration is one line.
- **An enum** has one name to a line, a level in, like a block's fields; the names with a value align on
  their `=`, and a counted name is only itself.
- **Blank lines**: at most one in a row, none just inside a brace, exactly one between top-level
  declarations -- a comment right in front of one stays with it. Imports stay together, and so do
  file settings. The file ends in exactly one newline.
- **Nothing else changes.** Numbers keep their spelling -- `0xCAFE` stays hex, `0b1010` binary -- and
  a comment's text is never touched, only moved to its line's indentation. A comment in the middle of
  a declaration stays there, and the declaration goes on on the next line, indented further.

A file that does not parse is not formatted at all: its errors are printed and it is left as it
was. One that parses and then fails a check is formatted like any other.

```sh
frame-dsl examples/*.fdsl --format           # rewrites any that are not in the layout
frame-dsl examples/*.fdsl --format --check   # changes nothing; names those that would change
frame-dsl - --format < in.fdsl > out.fdsl    # standard input to standard output
```

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
CRC polynomial and range, repeating a field typed by an enum, and conditions combining comparisons
(`&&`, `||`, ordering and masks, #126).
Calibrations have no lookup tables, domain checks or built-in functions yet.

`delimiter;` from the original sketch is **not** part of the language. It had no width and no defined
meaning; it can return as a defined construct if a real frame needs one.
