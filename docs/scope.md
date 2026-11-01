# Scope

What Frame-DSL is for, what it will not do, and how to tell whether a proposed change belongs in it.
This is the document to check a feature request against before designing it.

## The problem

Binary frame layouts change, and they change for reasons that have nothing to do with the program
carrying them -- a spacecraft's telemetry format is revised, a field grows, a reserved bit is given a
meaning. In the usual arrangement the layout is expressed as code: packed structs, bit masks, shift
arithmetic written by hand. That couples the layout to the build. Changing a frame means editing C,
recompiling, and re-deploying the program, even though the program's own logic did not change.

Frame-DSL moves the layout out of the code and into data. A frame is described in a small language,
compiled to a bytecode, and interpreted at run time by a C library. A program links the library once
and reads a definition file at startup. When a frame changes, the definition changes.

The bet is that this is worth paying an interpretation cost for, and that the cost can be made small
enough not to matter -- which is why the intermediate form is a bytecode rather than a syntax tree,
and why every field gets a fixed slot rather than an allocation.

## What it is responsible for

The core:

1. Transform an array of bytes into fields by a given blueprint, and fields into bytes.
2. Deny decoding only if the format or size is completely wrong: the bytes run out, a length passes
   its reservation or goes below zero, or no packet matches.
3. The user decides whether a value makes sense for what they are doing.

The utilities:

1. Offer calibration curves, so that users can easily turn bytes into something meaningful.
2. Be simple, and complete enough to carry the complex packets real hardware and systems use.

## Where it is used

Mostly on a simulated link, and that is a design input rather than a footnote. The common setting is
that one end of the conversation does not exist yet: a device that will send CAN frames at the
application being written, or the application that the device's own software needs something to talk
to.

A protocol specification, the formal document describing a communication format, says what goes on a
link. Each program on that link has its own `.fdsl`, written from that document, and they should
describe the same bytes. A firmware and an emulator of the device it talks to are the usual pair. A
misreading is then not shared, and integration shows the mismatch. Each program fills fields, and
checks that what it reads makes sense. A test can then change what one side sends: a wrong CRC, an
invalid read request, no replies at all, or unhealthy values in place of healthy ones.

That splits the project's use into two, and a feature request is worth checking against both:

- **As a library, while the communication is being written.** Fields set and read by name, packets
  encoded and decoded, the software asserting on what came back. The receiving side finds out which
  packet arrived from the bytes, unless the link carries that somewhere else, as a CAN ID does, and a
  `@decode: manual` packet is decoded by name.
- **As a tool, while the communication is being debugged.** A definition file and a run of raw bytes,
  with no program in between: what is this frame, and which field is wrong. This is why the CLI is
  shipped rather than kept as a test harness, and why its diagnostics distinguish "nothing matches"
  from "ran out of bytes".

## Deliberately a learning project

This exists to be built, in the same line as Nemu and NQC. That is a real constraint on decisions,
not a disclaimer:

- Where a choice is between *understanding the problem* and *shipping sooner*, it goes to
  understanding. Writing a parser by hand is the point; a parser generator would not be.
- Prior art (Kaitai Struct, ASN.1, Protocol Buffers, Wireshark dissectors, ERPC) is for stealing
  ideas from, and worth reading before designing anything here. It is not for replacing the project,
  and "X already does this" is not by itself an argument to stop.
- Being narrower than the alternatives is acceptable. Being *muddled* is not.

## In scope

What it is for:

- A `.fdsl` written from a protocol specification.
- Bytes to fields and back.
- Working out which packet arrived, or being told when the link carries that elsewhere.
- Passing every value through unjudged.
- Default values kept in a `.fdsl`.
- Calibrations and enums.
- Complex real packets.
- Rejecting a bad definition when it loads.
- Tools for people working with definitions.
- Running on Linux x86-64. Other processors might be desired later, so the runtime stays free of
  operating system calls.

And what that means, piece by piece:

**The language.** A human-readable syntax for declaring packets and their fields: widths in bits,
fixed values that identify a packet, fields whose length comes from another field, and bit ordering
and byte order configurable per file, per packet and per field.

**The compiler.** Lexer, parser, semantic checks, and a translation to a compact bytecode. The checks
matter as much as the parse: a definition whose packets cannot be told apart, or whose variable-length
field has no bound, should be rejected when the file is read rather than when a frame arrives.

**The runtime.** A C library holding an arena in which every field of every packet has its own
dedicated space, sized at load time. Encoding and decoding walk the bytecode and never allocate.
Multi-byte values are converted to host endianness on the way in and back to wire order on the way
out, so the arena holds host-native values and a caller never does byte-swapping by hand.

**The C API.** Setting and getting fields by name, encoding a named packet, and decoding a buffer
into whichever packet its fixed fields identify -- with an error mechanism a caller can actually use.

**Calibration curves.** A field may carry a named curve between its raw and engineering value,
readable and -- where the curve is a line or declares its inverse -- writable in engineering units,
callable by name, and exportable as C. See `manual/language.md`.

## Out of scope

Stated so that "could we also..." has an answer that is not a shrug:

- **Anything but Linux x86-64, for now.** The runtime is a shared library on one platform. Running on
  an embedded target is a plausible future, and is why the no-allocation rule exists, but nothing is
  being built or tested for it and no portability claim is made.
- **Computing or verifying a CRC or checksum.** It is a field like any other: the program fills it in
  and checks it, and can send a wrong one to see what the other end does.
- **Judging values.** Limits, ranges and plausibility belong to the program. The same reading is a
  failure to one program and exactly what another is injecting.
- **Generating C code.** Frame-DSL interprets a definition; it does not emit structs or accessors.
  Emitting code would defeat the requirement that frames change without recompiling.
- **Transport.** Reading, writing, framing, retrying and routing bytes are the calling program's
  business. This library turns a buffer into named fields and back.
- **Protocol behaviour.** Sequence numbers, acknowledgements, timeouts, state machines and sessions
  are all outside the language. A packet description describes a packet's layout, not a conversation.
- **Fault injection as a feature.** A wrong value is set like a right one. Building a malformed frame
  is not a job for this library either: a test patches the bytes it wants wrong.
- **Comparing definitions.** Checking two `.fdsl` files against each other, or against the protocol
  specification, is what integration does.
- **Schema evolution and negotiation.** There is no versioning of definitions, no compatibility
  checking between two versions of a frame, and no mechanism for two peers to agree on one.
- **A general-purpose serialisation format.** Frame-DSL describes layouts that already exist and are
  fixed by somebody else. It is not a wire format to choose for a new system.
- **Being a replacement for Kaitai Struct or ASN.1.** See above: narrower on purpose.

## Constraints these choices answer to

1. A definition is loaded at run time from a file; the program is not rebuilt when a frame changes.
2. Encoding and decoding do not allocate. Every field has a dedicated arena slot, sized at load time,
   which is why a variable-length field must declare a maximum.
3. The intermediate form is a bytecode meant to be interpreted quickly, not a tree walked with
   pointer chasing.
4. Errors that can be detected when the definition loads are detected then -- ambiguous packets,
   unbounded fields, a `valof()` naming a field that does not exist. A live link is a bad place to
   discover a typo.
