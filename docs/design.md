# Design

Why the language and runtime are shaped the way they are, and what the implementation has to do.
The language as a user sees it is in [`manual/language.md`](../manual/language.md); the boundaries of
the project are in [`scope.md`](scope.md).

## The pipeline

```
  definition file (.fdsl)
        |
        |  lexer          characters -> tokens
        v
      tokens
        |
        |  parser         tokens -> AST          (grammar.ebnf)
        v
       AST
        |
        |  semantic pass  checks + layout        (everything under "Checks" below)
        v
   checked model
        |
        |  codegen        -> bytecode + arena plan
        v
  loadable definition ------> runtime: encode / decode
```

Everything left of the runtime happens once, when the definition file loads. Everything the runtime
does per frame is walking a bytecode program against an arena that already exists.

**Why a bytecode rather than walking the AST.** Requirement 2 asks for something a computer can
interpret very fast. A tree walk chases a pointer per node, touches memory scattered across the
heap, and branches on a node type at every step. A flat instruction array is sequential, cache-warm,
and dispatches on a small dense enum. It is also the part of this project most worth building by
hand, which matters -- see the learning-project note in [`scope.md`](scope.md).

The bytecode is an internal format. It is not a file format, is not stable, and is not something a
user ever sees; the definition file is the only input.

## The arena

One allocation, holding a dedicated slot for every field of every packet.

It belongs to a **frame** rather than to the definition, so the allocation happens at
`framedsl_frame_new()` rather than at load. That keeps the definition const and shareable -- two
frames of one protocol can be in flight at once, and a decoder does not have to wait for an encoder
-- while changing nothing about the property that matters: once a frame exists, nothing allocates
again for as long as it lives.

The frame *is* the allocation. Its header, the slot descriptors and the bytes those slots point at
are three regions of one block, carved up at creation. Three separate mallocs would have behaved
identically; one means a reader checking the no-allocation claim has a single call to look at.

- A field of **64 bits or fewer** gets a `uint64_t`-sized slot and is a scalar.
- A field **wider than 64 bits** gets a byte-aligned slot of `ceil(width / 8)` bytes.
- A **`valof()`-sized** field gets exactly the bytes its `name[bytes]` reservation asks for -- not
  its current length -- plus a length, since the bytes actually present vary per frame.

This is what makes the reservation mandatory. Requirement 4 asks that each field have its own
dedicated space so that nothing allocates or frees; a field whose length is only known at decode time
can only have a dedicated space if there is a size to set aside. Making it an error to omit puts that
where the author can see it rather than in a runtime failure.

The reservation is written in **bytes**, alone among the numbers in this language, and binds to the
field's name rather than to its width: `data[128]: valof(len) * 8;`. Those are two different
statements -- how much room the field always gets, and how long it is in this frame -- and writing
them as one clause made a field look like it had a single, confused size. Bytes because a slot is a
run of memory, which has no sub-byte size to express, and because it then shares a unit with the
length field it usually mirrors.

**The arena holds host-native values.** Decoding converts from wire order into the host's; encoding
converts back. A caller reading a field never byte-swaps and never thinks about how the value sat on
the wire. This is the whole reason the endianness settings exist at load time rather than being a
concern of the calling program.

Sizing is worst case by construction: a packet's arena cost is fixed when the file loads, so a
definition that would not fit is rejected then, with a number, rather than discovered later.

**A scalar occupies a `uint64_t` whatever its declared width.** An 11-bit field costs eight bytes in
the arena rather than two. That is deliberate: reading one is then a load rather than a masked
assembly of bytes, and the arena is sized once at startup against a budget nobody is paying per
frame. Packing them would trade the hot path for memory that was never scarce.

**There is no "unset".** The arena starts zeroed and a slot keeps whatever was last written to it, so
a field nobody touched encodes as zero. Tracking which fields a caller had set would cost a bit per
field, a rule about what a reset does to it, and an encode that refuses a field somebody quite
reasonably meant to leave at zero.

**But a frame knows which packet it is holding.** That is not the same bookkeeping, and the
difference is whose values the slots contain. A slot the *caller* has not written is theirs to leave
at zero. A slot belonging to a packet that *did not arrive* holds a previous frame's value, or a zero
nobody set, and handing either over as an answer is inventing one -- which is the cost of the arena
having a slot per field of every packet rather than per field of one.

So a frame is in one of three states: holding the caller's values (new, reset, or written to),
holding one decoded packet, or holding nothing after a decode that stopped part-way. Reading a field
is refused in the last two unless the field belongs to the packet that arrived. It is one enum and
one comparison, not a bit per field, and it is checked at the accessor rather than costing the
encoder anything.

Writing a value returns the frame to the first state. A decoded frame somebody starts setting fields
on is no longer a record of what came in, and the alternative -- refusing to read back a value the
caller has just written -- is a rule that fires in the wrong direction. Encoding does not consult any
of this: it is told which packet it wants, so it is never the mistake this catches.

## Layout, bit ordering and endianness

Fields occupy consecutive bits in declaration order. There is no implicit padding and no automatic
alignment of *fields*: a field is whatever the specification being modelled says it is, which is
frequently not byte-aligned, and nothing is ever inserted to make one line up.

A *packet* is the exception, and the only one. Its fields must sum to a whole number of bytes
(check 13), because a frame leaves this library as bytes and a packet that stopped mid-byte would
have no unambiguous byte form -- its last byte would be part frame and part nothing, and every
caller would have to be handed a bit count alongside the buffer to know which. Rather than carry that
through the API, it is rejected when the definition loads. Nothing is padded to satisfy this: a
packet that does not add up is an error, and the author decides what belongs in the gap.

Two attributes, resolved per field as **field > block > packet > file**:

**`@bit_order`** -- how a field's bits enter the stream. `msb_first` (the default) writes the
most-significant bit of the value into the earliest bit position the field occupies; `lsb_first`
writes the least-significant bit first.

**`@byte_order`** -- how the bytes of a multi-byte value are arranged. `big_endian` is the default,
matching network order.

These are independent, which is why they are two settings rather than one. `byte_order` is only
meaningful for a field that is **byte-aligned and a whole number of bytes wide**; applying it
anywhere else is a load-time error rather than a line that quietly does nothing. That restriction is
deliberate: a 15-bit field has no bytes to reorder, and pretending otherwise would invent semantics
nobody could predict.

### Why the attributes reach field scope

Almost every file will set both once, at file scope, and never mention them again; a field that is
laid out differently from its neighbours reads as a modelling mistake. It is worth writing down why
the scope exists anyway, because the answer is "some protocols are like that" rather than "it seemed
more flexible":

- **ARINC 429.** A 32-bit word is transmitted LSB-first, *except* its 8-bit label, which is
  transmitted MSB-first. One field, reversed, inside a word that is not. There is no way to express
  an ARINC 429 word without a per-field `bit_order`, and avionics buses are not a hypothetical use
  for this project.
- **A CCSDS packet carrying a foreign-endian payload.** The primary header is big-endian by standard;
  the data field is frequently a structure copied out of a little-endian subsystem. This is the
  commonest real case by some distance.
- **Ethernet.** Frame bits go out LSB-first per octet, and the FCS MSB-first.

The cost is visible in this document: check 10, and the distinction between a setting written on a
field and one inherited, exist only because field scope does. That is judged worth paying, because
without it the language cannot describe frames it is specifically meant to describe.

The second case is usually a *run* of consecutive fields rather than one, which is what `block` is
for: a declaration that exists only to be something a `[ ... ]` group can attach to. A block is
flattened by the parser -- its fields are appended to the enclosing packet with consecutive offsets
and their own flat names -- so nothing downstream of the parse knows blocks exist, and neither the
layout pass, the checks nor the bytecode had to learn anything. The *named* form, which nests a
dotted name, is composition; see "Composition" below.

## Checks

Run when the definition loads, before anything can be encoded or decoded. Most exist because the
alternative is finding out on a live link.

**Naming**

1. Packet names are unique in a file, and so are named block names.
2. Field names are unique within a packet.
3. Reserved words are not usable as identifiers.

**Widths**

4. A width expression not containing `valof()` must evaluate to a positive constant, no more than
   half a gigabyte -- a width no sane frame reaches, held to stop a mistake from asking the arena for
   more than that in one field.
5. A width expression containing `valof()` must reserve a slot, written `name[bytes]`.
6. A reservation must be a positive constant expression, must not itself contain `valof()`, and is
   held to the same half-gigabyte as check 4 -- it becomes bits, and the arena allocates it.
7. A `valof()` must name a field **declared earlier in the same packet**. Reading a field requires
   knowing where it starts, which requires every field before it to be resolved already.

**Values**

8. A fixed value must fit in its field's declared width, and the field must be a scalar -- a bit
   string is bytes the caller handed over, not a number to hold one against.
9. A field may not have both `= <value>` and a variable width -- a width unknown until decode time
   cannot also be a discriminator.

**Settings**

10. A `@byte_order` written **on a field** may only appear on a field that is byte-aligned and a
    whole number of bytes wide. A `@byte_order` *inherited* from block, packet or file scope is not an error
    on such a field -- it simply does not apply to a field with no bytes to reorder. The distinction
    matters: a file-level `@byte_order` covers every field in the file, and most real frames contain
    sub-byte fields, so checking the effective setting rather than the written one would make the
    file-level form unusable. What is worth rejecting is an author writing a setting that cannot do
    anything, not a setting that is irrelevant here and meaningful two fields later.

**Identification**

11. Every packet has at least one fixed-value field.
12. No two packets in a file are indistinguishable.

**Framing**

13. A packet's fields sum to a multiple of 8 bits. Sub-byte fields are unaffected; it is the total
    that must land on a byte boundary. See "Deciding byte multiples" below for what this means when
    a `valof()` field makes the total depend on the frame.

**Storage**

14. A reservation appears only on a field whose width depends on another field. A constant-width
    field's slot is already known from its width, and a second number could only restate it or
    contradict it.
15. `@bit_order` and `@byte_order` may not be written on a bit string -- a field wider than
    `FRAMEDSL_SCALAR_MAX_BITS`, or a `valof()`-sized one. They describe how the bits of a value are
    arranged, and a bit string is the bytes the caller handed over. Inherited, either is simply
    irrelevant to that field and meaningful to the next one.
16. A `valof()` must name a **scalar** field. A width is arithmetic on a number, and anything wider
    than a scalar is a run of bytes with no single value to multiply. Without this the width would be
    computed from whatever the first eight bytes of a payload happened to be.

**Calibrations**

17. Calibration names are unique in a file.
18. A calibration named by `@cal:` or called from another calibration must exist. A typo would
    otherwise leave a field silently uncalibrated.
19. The only bare name a calibration's body may use is its own parameter -- and in an `inverse`, the
    inverse's parameter. Fields are not in scope: a curve belongs to no packet.
20. A calibration may not call itself, directly or through another. Calls are inlined at load time
    and there is no condition to end a recursion. Only a forward body's calls count: an `inverse` may
    call its own forward curve, which is a different function.
21. `@cal:` may only go on a scalar field. A bit string is bytes, not one number.
22. A calibration may not divide by a constant zero. A divisor that is zero only for some raw values
    is not a load-time error; that reading is refused at run time instead.

**Composition**

23. A field whose width is a name must name a block.
24. A block must be declared above everything that uses it, packet or block. The same rule makes a
    block containing itself, directly or through another, impossible to write.
25. An override must name a field of the block it is used with, and name it once. An instance's name
    is subject to check 2 like any field's, so `hdr: 8;` beside `hdr: Header;` is a duplicate even
    though `hdr` and `hdr.id` would not collide. A use with no instance name, `Header;`, pastes the
    block's fields in under their own names, and each is subject to check 2 in the packet or block it
    lands in -- reported at the use, naming the line of the field it clashes with.

**Imports**

26. An import must name a file that can be read. Its own problems are reported in it, by name.
27. Imports may not form a cycle, including a file importing itself.
28. Two different declarations of one name may not reach a file through flat imports. The same
    declaration reached twice -- two libraries importing a third -- is not a clash. A local
    declaration of an imported name is check 1 or 17, reported where it is written.

**Repetition**

32. A repeated block (`name: Block[count];`) must be the same size every time -- none of its widths
    may depend on another field -- and a whole number of bytes. The first is what puts element *i* at
    *i* times the element's size; the second is what keeps a packet whole bytes however many elements
    a frame carries, the half of check 13 a count cannot otherwise promise.
33. A repetition takes no overrides. An override fixes one field of one instance, and a repetition is
    many; a value every element carries is fixed in the block.

A repetition counted by a field is a field whose width has a `valof()` in it, so checks 5, 6 and 7
apply to its count and its reservation exactly as they do to any such field, and check 14 forbids a
reservation on a constant one.

**Shape**

29. A packet has at most 4096 fields, block and composition expansion included. Found by fuzzing
    (#59): several checks above, duplicate names foremost, are quadratic in field count -- fine for a
    real frame's dozens of fields, a multi-minute hang for a fuzzer-sized packet with hundreds of
    thousands of one-bit fields. Checked before any of them runs, so the packet that would make them
    slow is rejected in the time check 4 already takes.

**Warnings**

These do not fail a load. The file loads, encodes and decodes exactly as it would without them; the
warning is there for whoever is editing it, and `frame-dsl --check --werror` is how a CI job refuses
one. They share the numbering because they share the diagnostic, and a test asserts on the number
the same way.

30. A block, calibration or import declared in a file and used by nothing in it. Only in a file that
    declares at least one packet: a file of blocks and calibrations alone is a library, and its
    declarations -- what it imports included, since that is merged into what it offers -- are its
    exports. Anything declared in the file that names something counts as a use, even if it is itself
    unused, so of a chain only the outermost is reported. An imported file's own warnings are not
    repeated in the file importing it.
31. An attribute setting the value an enclosing *written* attribute already set -- a field's group
    repeating its packet's, a packet's repeating the file's. Reported at the inner one, which is the
    one saying nothing new. Only against something written: `@bit_order: msb_first;` at the top of a
    file restates the language's default, and that is documentation.

### Deciding distinguishability

Check 12 needs a definition precise enough to implement, and it is worth writing down because the
obvious informal version ("they have different fixed fields") is wrong.

For each packet, walk its fields in order accumulating a bit offset. While every field so far has a
constant width, the offset is constant too; once a `valof()`-sized field is passed, nothing after it
sits at a known position. For each fixed-value field at a constant offset, project its value onto
the stream bits it occupies -- *after* applying that field's `bit_order` and `byte_order`, since the
question is which stream bits are pinned, not which value bits are.

The result per packet is a pair of bit vectors over the constant-offset prefix:

- `mask` -- the stream bits this packet constrains
- `value` -- what it requires them to be

Two packets are **distinguishable** when they disagree somewhere both constrain:

```
distinguishable(P, Q)  <=>  (P.mask & Q.mask) & (P.value ^ Q.value)  !=  0
```

If that is zero, some frame could satisfy both packets' fixed fields, and the file is rejected
naming the pair. Note what this correctly rejects: two packets with *different* fixed fields that
never overlap constrain disjoint bits, so the `&` is empty and they are **not** distinguishable.
Having different discriminators is not enough; they have to contradict each other.

### Deciding byte multiples

Check 13 cannot be an addition, because a `valof()`-sized field has no width until a frame arrives.
Widths are therefore evaluated abstractly over **Z/8** -- each width's value modulo 8, where that is
knowable at all:

| Expression | Residue |
| --- | --- |
| a literal, or anything that folds to a constant | its value mod 8 |
| `valof(x)` | unknown |
| `a * b` | 0 if either side is a known 0; otherwise the product of the residues, if both are known |
| `a + b`, `a - b` | the sum or difference of the residues, if both are known |
| `a / b` | unknown unless the whole expression folds |

A packet passes when every field's residue is known and they sum to 0. So `valof(len) * 8` passes,
`(valof(n) + 1) * 8` passes -- one factor is a multiple of eight, which settles the product whatever
the other factor is -- and `valof(len) * 3` does not.

The analysis is **conservative**: it needs one factor that is itself a multiple of eight, so
`valof(n) * 2 * 4` is rejected despite always being a byte multiple. (`valof(n) * 3 * 8` is accepted
-- the outer factor settles it, whatever the inner product turns out to be.) That is the right
direction to be wrong in. The
alternative is deciding it per frame, which means a definition that loads cleanly and then fails on a
live link -- exactly what constraint 4 in [`scope.md`](scope.md) exists to prevent. An author whose
expression is rejected can always rewrite it so the multiple of eight is visible.

## Decoding

1. Compute the incoming frame's bits against every packet's `mask`/`value`. By check 12 at most one
   packet matches; if none does, the frame is unrecognised and that is the error.
2. Walk that packet's bytecode, field by field. Constant widths are known already; a `valof()` width
   is evaluated against the arena slot of the field it names, which has been decoded by then
   (check 7).
3. If a resolved width exceeds the field's reservation, stop: it is a decode error. Nothing is allocated,
   nothing is truncated, and the arena is left in a state the caller is told not to read.
4. Convert each field into host order and write it to its slot.

Encoding is the same walk in reverse, with one difference: fixed-value fields are written from the
definition, not from the arena, so a caller cannot emit a frame that fails to identify itself.

### Lending a buffer, and taking one

The encoder hands back a pointer into the frame, valid until the next call on it. That is the shape
the usual caller wants -- encode, give the bytes to a socket, forget them -- and it is why encoding
needs no buffer argument and can never fail for want of room.

It cannot, however, answer "refuse a buffer that is too small and say how much was needed", because
there is no caller buffer in it. A caller who already has somewhere the frame must end up -- a
transmit ring, a window into a larger message -- would otherwise copy into it themselves, having been
handed a pointer they did not want. So there is a second entry point that takes the buffer.

It encodes into the frame's own buffer and copies, which is worth justifying because it looks like a
copy too many. The length of a frame containing a `valof()` field is not known until the walk that
produces it has finished. Encoding straight into the caller's buffer therefore means either writing
into it before knowing whether it fits -- leaving a partial frame in a ring that something else is
about to send -- or walking every width twice to total them first. The first is wrong and the second
costs more than a memcpy of at most a few hundred bytes. Neither allocates; this is the cheaper of
the two correct options rather than a shortcut.

### Bit numbering

One statement, from which the rest follows:

```
stream bit i is bit 7 - (i % 8) of byte i / 8
```

Bit zero of a frame is the most significant bit of its first byte, which is the order a frame is
transmitted in. The mask and value a packet's fixed fields project onto use the same numbering, so a
packet's discriminator is **literally the bytes a frame of it must have** -- the decoder matches an
incoming buffer with an AND and a compare rather than reversing anything first.

A **scalar** is laid down by that rule under its resolved `bit_order`, after `byte_order` has
reordered its bytes where that means anything. A **bit string** is laid down as the bytes it was
given, each most significant bit first, shifted to wherever the field starts; neither attribute
applies to it, because it is a run of octets somebody handed over rather than a number with an order.
Check 15 rejects writing either on such a field, for the same reason check 10 exists: a line that
cannot do anything should say so rather than be ignored.

## The bytecode

A program is a flat array of field instructions, one per field, in declaration order. Each carries its
resolved `bit_order` and `byte_order`, whether it is a scalar, its fixed value if it has one, and how
its width is obtained -- either a constant, or a short postfix program evaluated on a small operand
stack whose only non-literal operand is "the value in slot N". `valof(len) * 8 - 32` becomes
`SLOT len; LIT 8; MUL; LIT 32; SUB`.

Three places the sketch turned out to be wrong, which is the expected outcome of building it rather
than a failure of the sketch:

**An instruction carries the field's index, not its slot in the arena.** The sketch assumed one arena
per definition. There is one per *frame*, and several frames can share a definition -- so a program
that named arena offsets would describe one frame rather than the definition it was compiled from.
The frame maps index to storage.

**A constant width compiles to no program at all**, rather than to a one-instruction one. It is the
overwhelmingly common case, and a number on the instruction costs the interpreter nothing where a
program would cost it a call and a loop.

**Constant subtrees are folded during compilation.** `valof(n) * (4 * 2)` is three instructions, not
five. The sketch did not say either way; doing it is two lines and the saving is paid per field per
frame rather than once.

The stack depth is computed from the emitted code -- not from the expression tree, which would be a
second opinion about a different thing -- and a program needing more than the evaluator's fixed stack
is rejected when the file loads. Nothing sane approaches it: `valof(len) * 8 - 32` reaches two.

The evaluator refuses a width that overflows or would go below zero. Both depend on a frame's
contents rather than on the definition, so neither can be settled when the file loads: a length field
claiming less than the header it counts is exactly how the second happens, and on a live link that is
a corrupt frame rather than a broken definition.

Like the arena, a program is **one allocation**, carved into the packet table, the field instructions
and the pool of width instructions. Compilation walks each width expression twice -- once to count
what it will emit, once to emit it -- which is what lets the count be known before anything is
allocated. The alternative was an allocation count that depended on the input.

## Composition

A named block is a group of fields declared once at file level and used in packets as an *instance*:
`hdr: Header { id = 0x02; };`. It exists so that the header every packet in a protocol shares is
written once, and each packet changes only what differs -- almost always the value that identifies it.

**Expanded before anything else sees it.** `compose_file()` runs between the parser and the checks and
replaces every instance with the block's fields, renamed under the instance (`hdr.sequence`), with
the `valof()` references inside renamed to match and the overrides applied as fixed values. What the
checks receive is packets of ordinary fields, so the layout pass, the distinguishability check, the
bytecode, the encoder and the decoder are unchanged -- and an instance costs nothing at run time that
writing its fields out would not. The arena does not know composition exists.

**Dotted names, not a tree.** An expanded field is called `hdr.sequence`, and `framedsl_find()` already
split `PACKET.rest` at the first dot and looked the rest up whole, so nested names needed no API
change. The resolver was written that way in #23 for exactly this.

**Named instances rather than splicing.** A block could have been spliced into a packet under its own
field names. Naming the instance was chosen because it lets one block appear twice in a packet (an
accelerometer and a gyroscope are both a `Vector3`), and because two blocks spliced side by side can
collide in ways that only show up far from where they were written.

**Overrides fix values, and nothing else.** A block's widths, order and attributes are the same in
every use: an attribute resolves where the block is declared, and a group in front of a use is an
error. A per-use width or byte order would make "a packet built from `Header`" say nothing about the
packet's layout, which is most of what reusing `Header` is for.

**Declared before use.** A block must appear above anything that uses it. Besides reading top to
bottom, that makes recursion unrepresentable: a block is declared from its closing brace, so nothing
inside it can use it. Each block is also expanded on its own before any packet is, so a mistake inside
a block is reported once, at the block, and a block no packet uses yet is still held to checks 23 to
25. Expansion is bounded (`COMPOSE_FIELD_MAX`) so that blocks nested two-by-two cannot make loading
explode.

## Repetition

`name: Block[count];` (#55) lays a named block out `count` times in a row, the count a constant or a
`valof()` of a field above.

**One field, not many.** `compose_file()` turns a repetition into a single field whose width is
`count * element_bits` -- with the reservation written on it, when the count is a field's -- and
marks it with the block it repeats. So the checks, the layout pass, the bytecode, the encoder and the
decoder see a bit string like any other: a counted repetition is a `valof()`-sized field, bounded by
its reservation and refused past it, and a constant one is a fixed-width field. Nothing in the walk
learned anything new. The element is laid out once, apart from the packets, as an `ElementProgram`:
each of its fields' offset inside an element, width, orders, fixed value and curve.

**Elements are views.** A repetition's slot holds its elements exactly as they are on the wire, one
after another. A member of element *i* is `i * element_bits + offset` into it, and the accessors read
and write it there, in its own bit and byte order -- so there is no second copy of anything to keep in
step, and a decoded frame's elements are readable the moment the bytes are copied in. That is why a
repeated block must be one size and whole bytes (check 32): it is what makes the offset arithmetic.

**Two ways to one element.** `framedsl_find()` takes `PKT.samples[3].value`, which is what a person
writes, and `PKT.samples.value`, an id with no element chosen, which `framedsl_element()` turns into
element *i* without formatting a name per iteration. `FramedslFieldId` grew an `element` and a
`member` for it; a whole-field id has member 0. `framedsl_count()` answers how many elements a frame
holds by running the count's width program on the frame's current values -- the arithmetic encode
itself will do.

**What encode adds.** The run of elements is copied from the slot like any bit string, except that its
length is not compared against the count: elements are set one member at a time, so there is no length
to disagree, and an element nobody set is zeros like any unset field. Then every element's fixed values
are written over it from the definition, as a fixed field's always are.

## Imports

`import "path";` and `import "path" as name;` bring the blocks and calibrations of another file into
this one. The case they are for is a family of devices, or a vendor, sharing headers, trailers and
curves that should be written once.

**Resolved between the parser and composition.** `imports_resolve()` reads each imported file,
resolves *its* imports, composes it and checks it -- packets and all -- entirely on its own. Only if it
is clean are its blocks and calibrations merged into the importing file, ahead of the file's own
declarations. Everything after that sees one file of ordinary declarations; composition, the checks
and the runtime needed nothing new.

**Checked alone, reported alone.** A mistake in a library is reported once, at its own position and
under its own name -- diagnostics gained a `file` for this -- however many files import it. The
alternative, checking imported declarations as part of the importing file, would report the same
problem once per importer, at positions in a file the reader is not looking at. For the same reason
named blocks are now checked on their own like packets (minus checks 11 and 13, which only make
sense of a whole frame), so a block nothing uses yet is still held to the rules.

**Blocks and calibrations, not packets.** Importing packets would let a file change what a definition
decodes by importing something, which is the kind of action at a distance the distinguishability
check exists to prevent. A file can be a protocol and a library at once; only its reusable half
travels.

**Flat, or under a name.** A flat import is what most trees want: one bus, one `Header`. Two libraries
that both declare `Header` are the case for `as`, which prefixes every name the file brings -- and,
since a library's own references are all to names in the same scope, every reference inside it gets
the same prefix and keeps meaning what it meant. Each merged declaration carries the real path of the
file that declared it, which is how a diamond (two libraries importing a third) is told apart from a
clash: same name, same origin is the same declaration reached twice.

**Layout stays where it was declared.** An imported block's attributes resolved in its own file, as
any block's do. A little-endian device importing a big-endian header gets a big-endian header.

**Paths relative to the importing file**, and no search path. A tree of definition files is then
self-contained and moves as a unit; a search path would make a definition's meaning depend on the
environment it is loaded in. Imports are still read when the definition loads, so a host program
changes frames without recompiling, which is the property the whole library rests on.

## Calibrations

A curve is a per-field transformation between the raw value on the wire and the engineering value a
caller sees, applied on the way out of a read and on the way into a write -- on the boundary the
arena already drew between wire order and host values. The layout rules did not change, which is the
property that was worth preserving when this section only said where curves would attach.

**The arena holds raw values only.** An engineering value is computed when it is asked for. The
alternative -- holding both -- would have meant a second slot per calibrated field, a decode that
runs every curve whether anybody wants it or not, and two copies that could disagree. This way a
field without a curve, or a decode nobody reads in engineering units, costs exactly what it did.

**A calibration is its own function, not a field attribute's expression.** `cal name(raw) = ...;` at
file scope, attached with `@cal: name;`. Declaring it once and naming it means one curve serves every
field that shares a sensor, a curve can call another (`current` is `volts` over a shunt), and a
program can call a curve by name with no field at all -- to turn a limit in volts into counts, say.

**Compiled two ways.** When the file loads, each curve is analysed for being a line: `a * x + b`,
through negation, constant factors, constant divisors and calls to other lines. Most real curves are,
and those become two numbers and two floating-point operations. Anything else becomes a postfix
program over a fixed stack of doubles, shaped like a width program: constant subtrees folded, and
every call inlined with its argument held in a register, so a program never calls anything. Inlining
is bounded (`CAL_CODE_MAX`, `CAL_REGISTER_MAX`) so that a file cannot make loading exponential.

Folding a line reassociates the arithmetic -- `raw * 3.3 / 4095` runs as `raw * 0.000805...` -- so a
result may differ from the source's literal order in the last bit. That is accepted: the compiled form
*is* the definition of the value, and everything else is generated from it.

**The inverse.** Encoding from an engineering value needs the curve backwards. A line is inverted
automatically, as `(y - b) / a` rather than as another line, which would round twice. Anything else
must declare `inverse (eng) = ...;` -- general inversion (bisection over the raw range, say) was
rejected because it needs the curve to be monotonic, which cannot be checked from the source, and a
wrong answer from a non-monotonic curve would be silent. An `inverse` somebody wrote wins even over a
line: they may know something about rounding that solving the line does not. The raw value is rounded
to the nearest integer, half away from zero, which bounds a round trip at half a raw step.

**Ahead of time.** `framedsl_emit_c()` writes each compiled curve as a `static inline double` function:
a line as `a * raw + b` with the folded constants printed to 17 significant digits (enough for a
double to read back as itself), code as the postfix program turned back into fully parenthesised
infix. Because it comes from the compiled form, it is the same operations in the same order, and a
test compiles it and compares hex-float output against the library bit for bit. What can still break
that is the C compiler reassociating or fusing, which `-ffast-math` and FMA contraction do; neither is
on under `-std=c11`.

A JIT was considered and not built: the line fast path already removes the interpreter from most
curves, and the emitted C removes it from the rest for anybody who can compile.

### What a decode reports

Three failures, kept apart because a program responds to them differently. `UNRECOGNISED` is nothing
here matching -- noise, or a sender running a newer file -- and is something to count and drop.
`TRUNCATED` is a packet matching and the bytes then running out, which means the transport is handing
over the wrong bytes. `RANGE` is a length field claiming more than its field reserved, which means the
frame is corrupt. A single "decode failed" would leave a caller unable to tell a noisy link from a
broken one.

A decode that fails also leaves the frame holding *nothing*, so every field reads back as absent
rather than as whatever the walk had written before it stopped. "Do not read the arena after a failed
decode" was previously a sentence in the manual, and the caller it is addressed to -- the one who did
not check the return value -- is precisely the caller who does not read the manual either.

A decode also reports how many bytes the frame occupied. A packet with a `valof()` field has no length
until it has been decoded, so without that a caller reading a stream would have to re-implement the
length field's arithmetic to find where one frame ended and the next began -- which is precisely the
duplication this library exists to remove. Bytes past the frame are left alone.

## Deliberately unsettled
