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

One allocation at load time, holding a dedicated slot for every field of every packet.

- A field of **64 bits or fewer** gets a `uint64_t`-sized slot and is a scalar.
- A field **wider than 64 bits** gets a byte-aligned slot of `ceil(width / 8)` bytes.
- A **`valof()`-sized** field gets `ceil(max / 8)` bytes -- its declared bound, not its current
  length -- plus a length, since the bytes actually present vary per frame.

This is what makes the mandatory `max` mandatory. Requirement 4 asks that each field have its own
dedicated space so that nothing allocates or frees; a field whose length is only known at decode
time can only have a dedicated space if there is a bound to reserve. Making `max` a syntax error to
omit puts that where the author can see it rather than in a runtime failure.

**The arena holds host-native values.** Decoding converts from wire order into the host's; encoding
converts back. A caller reading a field never byte-swaps and never thinks about how the value sat on
the wire. This is the whole reason the endianness settings exist at load time rather than being a
concern of the calling program.

Sizing is worst case by construction: a packet's arena cost is fixed when the file loads, so a
definition that would not fit is rejected then, with a number, rather than discovered later.

## Layout, bit ordering and endianness

Fields occupy consecutive bits in declaration order. There is no implicit padding and no automatic
alignment: a frame is whatever the specification being modelled says it is, which is frequently not
byte-aligned.

Two settings, resolved per field as **field > packet > file**:

**`bit_order`** -- how a field's bits enter the stream. `msb_first` (the default) writes the
most-significant bit of the value into the earliest bit position the field occupies; `lsb_first`
writes the least-significant bit first.

**`byte_order`** -- how the bytes of a multi-byte value are arranged. `big_endian` is the default,
matching network order.

These are independent, which is why they are two settings rather than one. `byte_order` is only
meaningful for a field that is **byte-aligned and a whole number of bytes wide**; applying it
anywhere else is a load-time error rather than a line that quietly does nothing. That restriction is
deliberate: a 15-bit field has no bytes to reorder, and pretending otherwise would invent semantics
nobody could predict.

## Checks

Run when the definition loads, before anything can be encoded or decoded. Most exist because the
alternative is finding out on a live link.

**Naming**

1. Packet names are unique in a file.
2. Field names are unique within a packet.
3. Reserved words are not usable as identifiers.

**Widths**

4. A width expression not containing `valof()` must evaluate to a positive constant.
5. A width expression containing `valof()` must declare `max`.
6. `max` must be a positive constant expression, and must not itself contain `valof()`.
7. A `valof()` must name a field **declared earlier in the same packet**. Reading a field requires
   knowing where it starts, which requires every field before it to be resolved already.

**Values**

8. A fixed value must fit in its field's declared width.
9. A field may not have both `= <value>` and `max` -- a width unknown until decode time cannot also
   be a discriminator.

**Settings**

10. A `byte_order` written **on a field** may only appear on a field that is byte-aligned and a
    whole number of bytes wide. A `byte_order` *inherited* from packet or file scope is not an error
    on such a field -- it simply does not apply to a field with no bytes to reorder. The distinction
    matters: a file-level `byte_order` covers every field in the file, and most real frames contain
    sub-byte fields, so checking the effective setting rather than the written one would make the
    file-level form unusable. What is worth rejecting is an author writing a setting that cannot do
    anything, not a setting that is irrelevant here and meaningful two fields later.

**Identification**

11. Every packet has at least one fixed-value field.
12. No two packets in a file are indistinguishable.

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

## Decoding

1. Compute the incoming frame's bits against every packet's `mask`/`value`. By check 12 at most one
   packet matches; if none does, the frame is unrecognised and that is the error.
2. Walk that packet's bytecode, field by field. Constant widths are known already; a `valof()` width
   is evaluated against the arena slot of the field it names, which has been decoded by then
   (check 7).
3. If a resolved width exceeds the field's `max`, stop: it is a decode error. Nothing is allocated,
   nothing is truncated, and the arena is left in a state the caller is told not to read.
4. Convert each field into host order and write it to its slot.

Encoding is the same walk in reverse, with one difference: fixed-value fields are written from the
definition, not from the arena, so a caller cannot emit a frame that fails to identify itself.

## The bytecode, sketched

Not implemented, and deliberately not nailed down further than the shape -- the design that survives
contact with the encoder is the one worth writing down.

A program is a flat array of field instructions, one per field, in declaration order. Each carries:
its slot in the arena, its resolved `bit_order` and `byte_order`, and how its width is obtained --
either a constant, or a short postfix expression program evaluated on a small operand stack, whose
only non-literal operand is "the value in slot N".

A postfix expression keeps the arithmetic from needing a tree at run time, and the stack depth is
bounded by the expression's own nesting, which is known when it is compiled. `valof(len) * 8 - 32`
becomes roughly `SLOT len; LIT 8; MUL; LIT 32; SUB`.

## Where calibration curves will attach

In scope as a concern, not designed. The place they attach is decided, so that adding them later is
not a redesign: a curve is a per-field transformation between the raw value on the wire and the
engineering value a caller sees, applied after decode and before encode, on the boundary the arena
already draws between wire order and host values.

That means a field instruction gains an optional transform reference, and the arena may need to hold
an engineering value alongside the raw one. It means the language gains a per-field attribute. It
does not mean anything about the layout rules changes, which is the property worth preserving now.

## Deliberately unsettled

**The C API.** The sketch in issue #2 -- `framedsl_set_u32("PKT.field", v)` with errors accumulated
by `|=` -- does not survive scrutiny: it has no context handle, so there can only ever be one loaded
definition; it looks a field up by string on every call; and `|=` over error codes turns two
different failures into a third value that means neither. It needs a context handle, field handles
resolved once, a caller-owned encode buffer, and an error mechanism that reports *which* call failed.
That is a runtime concern rather than a language one, and it has its own issue.

**Multiple definition files**, includes, and namespacing. One file, one load, for now.
