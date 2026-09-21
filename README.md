# Frame-DSL

A small language for describing the layout of binary packets, and a C runtime that encodes and
decodes them from that description at run time.

The idea is that the shape of a frame -- its fields, their widths in bits, which values identify it,
which field gives another field's length -- is data, not code. A program links the runtime once,
loads a definition file at startup, and addresses fields by name:

```c
FramedslFieldId len, payload;
framedsl_find(definition, "TM_DATAGRAM_02.len",  &len);
framedsl_find(definition, "TM_DATAGRAM_02.data", &payload);

framedsl_set_u64(frame, len, 2);
framedsl_set_bytes(frame, payload, data, sizeof(data));
```

...encodes it, and reads one back without being told what arrived:

```c
size_t length;
const uint8_t *bytes = framedsl_encode(frame, "TM_DATAGRAM_02", &length, NULL);
send(socket, bytes, length, 0);

const char *packet = framedsl_decode(frame, received, received_length, &consumed, NULL);
```

When a frame changes, the definition file changes. The program does not get rebuilt.

> **Status: early, and now end to end.** A definition file loads and is checked; a program sets
> fields by name, encodes a frame, and decodes one back without being told which packet arrived. That
> is the first milestone in [`docs/scope.md`](docs/scope.md). Signed and floating-point fields, enums,
> conditionals, repetition, composition, CRC and calibration curves are all still absent, each with
> its own issue. Nothing here is stable, and nothing is published.

## The two ways this gets used

Both of them are about a device that is not in front of you. The layouts here are the telemetry and
telecommand packets some piece of hardware speaks, and the usual reason to want them in a file rather
than in C is that one end of the link is being *simulated*: something has to stand in for the device
sending CAN frames at the application being written, or stand in for the application, so that the
software on the other side has something real to talk to.

**While developing, as a library.** The program under development links the runtime, loads a
definition at startup, builds commands by setting fields by name, and decodes what comes back into
something it can act on -- "did the device answer the sequence I sent, and is the reading over the
limit" rather than "what is in byte 7". Both ends of a simulated link load the same definition file,
so the simulator and the software under development cannot disagree about a layout, and changing the
layout is editing one file rather than two programs.
[`examples/uart_device.c`](examples/uart_device.c) is exactly that: a UART driver whose device does
not exist, in the shape a real one has. `make example && ./build/uart_device`.

**While debugging, as a standalone tool.** When the communication already works and the question is
why *this* frame was rejected, or what the unexpected reply actually contained, there is nothing to
write: `build/frame-dsl` takes the definition file and the raw bytes off the link and says what they
are. The definition you already have and a hex string pasted off a capture are the whole input -- see
[The tool](#the-tool). It goes the other way too: name a packet and the fields to put in it, and it
prints the bytes to send by hand.

## What it looks like

A definition file declares packets. Widths are in bits, everywhere:

```
@bit_order:  msb_first;
@byte_order: big_endian;

pkt TM_DATAGRAM_02 {
  some_header: 8 = 0xAA;        # a fixed value: this is what identifies the packet
  len:         32;              # counts payload bytes
  data[128]:   valof(len) * 8;  # 128 bytes of arena; the width is worked out per frame
  crc:         16;
};
```

Three things that are load-bearing rather than incidental:

- **Every width is in bits**, and `valof(x)` is the referenced field's raw value. Length fields
  rarely count exactly what follows them, so the arithmetic is written at the use site instead of
  being inferred from a convention.
- **A `valof()` field must reserve a slot**, written `name[bytes]` on the field's name. The arena
  sets that much aside up front, so encoding and decoding never allocate, and a frame claiming more
  than the reservation is an error rather than a surprise. It is the one number in the language
  written in bytes: it sizes memory, not a position in a frame.
- **A decodable packet must carry at least one fixed-value field.** Definitions whose packets cannot
  be told apart are rejected when the file loads, not when a frame arrives on a live link.

A header shared by many packets is written once, as a named block, and each packet fixes only what
differs -- usually the id that tells them apart:

```
block Header { id: 8; sequence: 16; };

pkt TC_PING  { hdr: Header { id = 0x01; }; };
pkt TM_REPLY { hdr: Header { id = 0x81; }; status: 8; };   # fields: hdr.id, hdr.sequence, status
```

Headers, trailers and calibrations shared by several files live in a library that each of them
imports -- `import "common/link.fdsl";`, or `import "vendor.fdsl" as vendor;` to keep two libraries'
names apart. See [`examples/imported_device.fdsl`](examples/imported_device.fdsl).

The full language reference is in [`manual/`](manual) -- including the [C API](manual/api.md) --
and the design behind it in [`docs/`](docs).

## The tool

`build/frame-dsl` reads a definition and says what is in it:

```sh
frame-dsl examples/packet_family.fdsl
```

Give it bytes and it decodes them instead, working out which packet they are from the frame itself:

```sh
$ frame-dsl examples/packet_family.fdsl -d "01 00 01  03 00 02 00 02 AA BB"
# Decoded 3 bytes
pkt TC_PING {
    type           1 (0x1)
    seq            1 (0x1)
}
# Decoded 7 bytes
pkt TC_PAYLOAD {
    type           3 (0x3)
    seq            2 (0x2)
    len            2 (0x2)
    body           [2 bytes] AA BB
}
2 frames, 10 bytes
```

The hex may be written however the thing you copied it from writes it -- spaces, colons, commas,
dashes, `0x` prefixes, or nothing at all. An odd number of digits is a typo and is refused rather
than padded: padding it would change the frame you are asking about.

Frames back to back are walked one at a time, which is the only way to read a capture containing a
packet whose length is a field inside it. A frame nothing matches, one that runs out of bytes, and
one whose length field claims more than its field reserved are three different messages, because
they mean three different things to whoever is holding the capture.

Name a packet and some field values and it encodes them instead, printing the frame as hex on
stdout -- the same spacing `-d` accepts, and what `xxd -r -p` reads, so it can be pasted or piped:

```sh
$ frame-dsl examples/packet_family.fdsl -e TC_PAYLOAD seq=2 len=2 body=AABB
03 00 02 00 02 AA BB
$ frame-dsl examples/packet_family.fdsl -e TC_PING seq=1 | xxd -r -p > ping.bin
```

A scalar takes decimal or `0x` hex; a bit string takes hex, written any way `-d` takes it. A field
not named encodes as zero, as it does through the library, and each one is named on stderr so that
a field forgotten does not pass for a field meant to be zero; `-q`/`--quiet` stops that. `-r`/`--report` also prints the frame
field by field, as `-d` would, on stderr so that stdout stays just the bytes.

A field the packet does not have, a value too wide for its field, a packet the definition does not
contain, and a fixed-value field -- the definition sets that one, not you -- are each refused with
their own message. A `valof()` field and the field that sizes it must agree: give a payload and its
length both, or neither, and it will not work the length out for you.

A field with a calibration shows its engineering value beside the raw one, and `--emit-c` writes
every calibration in the file out as C, for a program that wants the curves compiled in:

```sh
$ frame-dsl examples/calibrated_sensor.fdsl -d "10 80 07 FF 03 20 07 FF"
...
    board          800 (0x320) = 10 [celsius]
...
$ frame-dsl examples/calibrated_sensor.fdsl --emit-c > curves.h
```

See [Calibrations](manual/language.md#calibrations) for how a curve is written.

Exit status is `0` for success, `1` when the definition, the bytes or the field values are wrong, and
`2` when the invocation is -- including `-d` and `-e` together, which are two questions at once.

Emacs users get syntax highlighting, comments and indentation for `.fdsl` files from
[`editors/frame-dsl-mode.el`](editors/frame-dsl-mode.el); [`editors/README.md`](editors/README.md)
says how to install it, and [`docs/highlighting.md`](docs/highlighting.md) says why it is a
hand-written mode rather than a tree-sitter grammar or a language server.

## Building

Requires a C11 compiler, `make` and `tar`. **No network, and no credentials.**

```sh
make        # build build/frame-dsl
make test   # build and run the test suite
```

Formatting is clang-format's, from `.clang-format`, and `make format` applies it. That file uses
options introduced in **clang-format 21**, and an older binary does not merely format differently --
it cannot read the file at all. Where `clang-format` is older than that (Debian bookworm and the CI
image both ship 14), name a newer one:

```sh
make format CLANG_FORMAT=clang-format-22
```

`make` wires `.githooks/pre-commit` in, which formats staged sources on the way into a commit. That
is a convenience: CI runs `make format-check` before it builds, and that is what actually enforces
it.

`make` also writes `compile_commands.json` at the repository root, so clangd and anything else
speaking that format can index the project with no further setup -- including the generated test
registries, and the deliberate absence of `src/` from the system tests' include path. It is
gitignored: the paths in it are absolute, so it belongs to one checkout on one machine. Nothing needs
to be installed to produce it; the Makefile already knows every flag it compiles with.

The three CLibs libraries this builds against are vendored: their release tarballs are committed
under `third_party/clibs/`, and unpacked into `build/` as part of the build. Nothing is downloaded,
so a clone builds on a machine with no route to the Forgejo instance and CI needs no secret.

To move to a newer CLibs, run the updater by hand and commit what it replaces:

```sh
scripts/vendor_clibs.sh          # refresh the vendored version
scripts/vendor_clibs.sh v0.2.0   # or move to a specific one
```

That script is the only thing here that talks to the network, and the only thing that needs a token
-- CLibs is private. `bytewise --conf-repo .` links one in; `bytewise --status .` shows what is
configured.

| Target                  |                                                        |
|-------------------------|--------------------------------------------------------|
| `make`                  | build `build/frame-dsl`                                |
| `make example`          | build `build/uart_device`, the worked example           |
| `make lib`              | build `build/libframedsl.a` to compile your own against |
| `make deps`             | unpack the vendored CLibs libraries into `build/deps/` |
| `make test`             | run both levels of tests                               |
| `make test-unit`        | unit tests only                                        |
| `make test-system`      | system tests only                                      |
| `make test-editors`     | the Emacs mode's tests; skipped when Emacs is absent   |
| `make format`           | reformat every first-party C source in place           |
| `make format-check`     | check the formatting without changing anything         |
| `make compile-commands` | regenerate `compile_commands.json` alone               |
| `make clean`            | remove `build/` and `compile_commands.json`            |

## Layout

| Path                 |                                                                                    |
|----------------------|------------------------------------------------------------------------------------|
| `include/`           | `framedsl.h`, the public API and the only header a user includes                   |
| `src/`               | the compiler, the runtime and the command-line front end                           |
| `test/`              | one user story per file, `NN_name.c`; the runner registers them from the filenames |
| `test/corpus/`       | definition files for the table in `test/system/7_corpus.c` -- a row per combination |
| `manual/`            | the language reference -- what someone writing a definition file needs             |
| `docs/`              | design documents -- why the language and runtime are shaped the way they are       |
| `examples/`          | example definition files, doubling as the parser's fixtures                        |
| `editors/`           | `frame-dsl-mode.el`, an Emacs major mode for `.fdsl` files                          |
| `scripts/`           | `vendor_clibs.sh`, which updates the committed CLibs tarballs                      |
| `templates/`         | issue templates; `GUIDELINES.md` requires issues to follow them                    |
| `third_party/clibs/` | the CLibs release tarballs, committed and unpacked at build time                   |

## Working on this repository

Read [`GUIDELINES.md`](GUIDELINES.md) first. It is short, and it is binding rather than advisory: it
covers branch and PR naming, the requirement that every PR reference an issue, the `state/*` labels
that gate who may act on an issue, and the rule against self-attribution anywhere in the repository.

[`CHANGELOG.md`](CHANGELOG.md) records what has landed. Releases draw their notes from it.
