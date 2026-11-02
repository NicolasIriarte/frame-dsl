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

> **Status: early, and end to end.** A definition file loads and is checked; a program sets fields by
> name, encodes a frame, and decodes one back without being told which packet arrived. What the
> project is for, and what it is not, is in [`docs/scope.md`](docs/scope.md). The C API may still change before 1.0.

## The two ways this gets used

Both of them are about a device that is not in front of you. The layouts here are the telemetry and
telecommand packets some piece of hardware speaks, and the usual reason to want them in a file rather
than in C is that one end of the link is being *simulated*: something has to stand in for the device
sending CAN frames at the application being written, or stand in for the application, so that the
software on the other side has something real to talk to.

**While developing, as a library.** The program under development links the runtime, loads a
definition at startup, builds commands by setting fields by name, and decodes what comes back into
something it can act on -- "did the device answer the sequence I sent, and is the reading over the
limit" rather than "what is in byte 7". Each end of a link has its own definition file, written from
the protocol specification, so a misreading is not shared and integration shows the mismatch, and
changing a layout is editing a file rather than a program.
[`examples/uart_device.c`](examples/uart_device.c) is exactly that: a UART driver whose device does
not exist, in the shape a real one has. `make example && ./build/uart_device`.

**While debugging, as a standalone tool.** When the communication already works and the question is
why *this* frame was rejected, or what the unexpected reply actually contained, there is nothing to
write: `build/frame-dsl` takes the definition file and the raw bytes off the link and says what they
are. The definition you already have and a hex string pasted off a capture are the whole input -- see
[The tool](#the-tool).

## What it looks like

A definition file declares packets. Widths are in bits, everywhere:

```
@bit_order:  msb_first;
@byte_order: big_endian;

pkt TM_DATAGRAM_02 {
  some_header: 8 := 0xAA;       # a fixed value: this is what identifies the packet
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

pkt TC_PING  { hdr: Header { id := 0x01; }; };
pkt TM_REPLY { hdr: Header { id := 0x81; }; status: 8; };   # fields: hdr.id, hdr.sequence, status
```

A field whose values mean something is typed by an enum, and a discriminator reads as what it means:

```
enum PktType: 4 { HK = 0x1; EVENT; };
enum TxLevel: 3 { OFF; LOW; MID; HIGH; MAX = 7; };

pkt TM_ANTENNA { type: PktType := HK; level: TxLevel; pad: 1; };   # level is 3 bits
```

Part of a frame that is there only sometimes -- a secondary header a flag announces -- is conditional,
and a decoded frame says which parts it carried:

```
block Time { seconds: 32; subsecs: 16; };

pkt TM_SPACE {
  id:      8 := 0x08;
  sec_hdr: 8;
  [ @if: valof(sec_hdr) == 1; ]
  time:    Time;                                       # read as absent from a frame without it
  crc:     16;
};
```

Headers, trailers, calibrations and enums shared by several files live in a library that each of them
imports -- `import "common/link.fdsl";`, or `import "vendor.fdsl" as vendor;` to keep two libraries'
names apart. See [`examples/imported_device.fdsl`](examples/imported_device.fdsl).

The full language reference is in [`manual/`](manual) -- including the [C API](manual/api.md) --
and the design behind it in [`docs/`](docs).

## The tool

`build/frame-dsl` reads a definition and says what is in it:

```sh
frame-dsl examples/packet_family.fdsl
```

`frame-dsl --help` lists every mode and option, with examples; what follows is the tour.

Bash and zsh complete it: `frame-dsl --<TAB>` offers the flags and a definition argument `.fdsl` files.
`--list-packets` prints a file's packet names one per line, for any script to read. `make install`
puts both completions where their shells look; to try them from the checkout,
`source completions/frame-dsl.bash`.

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

The tool reads and does not write: it does not encode. A frame to send is built by a program through
the library -- in C as in [`examples/uart_device.c`](examples/uart_device.c), or from a
script through the [Python bindings](#from-python).

A field with a calibration shows its engineering value beside the raw one, and `--emit-c` writes
every calibration in the file out as C, for a program that wants the curves compiled in:

```sh
$ frame-dsl examples/calibrated_sensor.fdsl -d "10 80 07 FF 03 20 07 FF"
...
    board          800 (0x320) = 10 [celsius]
...
$ frame-dsl examples/calibrated_sensor.fdsl --emit-c > curves.h
```

See [Calibrations](manual/language.md#calibrations) for how a curve is written. A field typed by an
[enum](manual/language.md#enumerations) shows its name the same way -- `level 3 (0x3) = HIGH
[TxLevel]` -- and one the enum does not list as `<unlisted>`, rather than failing the frame.

Results go to stdout and problems to stderr, so either can be piped: a failed decode says `frame-dsl: at
byte 3: ...` there. On a terminal that name is red, as is `error:` in a problem with a definition, and
`warning:` is yellow. Results are never coloured, and nothing is in a pipe or with `NO_COLOR` set.
Exit status is `0` for success, `1` when the definition or the bytes are wrong, and `2` when the
invocation is.

### Checking a file

`--check` loads each file named and prints nothing but its problems, one per line, in the shape every
editor and build tool already reads:

```sh
$ frame-dsl examples/*.fdsl broken.fdsl typo.fdsl --check
broken.fdsl:4:3: error: expected ';', found the name 'data'
typo.fdsl:7:3: error: 'hdr' uses 'Heder', which is not a block [check 23]
```

No colour, no summary, and nothing at all for a clean file. A problem inside an imported file names
that file. The exit status is `0` when nothing is wrong, `1` when something is, and `2` when a file
could not be read; `--werror` makes a warning count as wrong too, for a CI job that wants none.

A definition named `-` is read from standard input, and `--stdin-name <path>` says which file it is:
diagnostics are attributed to that path and imports resolve from beside it. That is how an editor
checks a buffer as it is typed, before it is saved:

```sh
frame-dsl - --check --stdin-name src/frames/device.fdsl < unsaved-buffer
```

`make install PREFIX=~/.local` puts `frame-dsl` on the PATH, which is where every editor integration
looks for it, along with its Bash and zsh completions. zsh looks in `share/zsh/site-functions` only
under `/usr` and `/usr/local`; for `~/.local`, add `fpath=(~/.local/share/zsh/site-functions $fpath)`
before `compinit`.

### Formatting a file

`--format` rewrites each file named into the one layout the project uses, in place -- see
[Layout](manual/language.md#layout) for what it is. `--format --check` changes nothing and names the
files that would change, exiting `1` if there are any, which is what CI runs. `-` formats standard
input to standard output, the form an editor uses to format a buffer on save. A file that does not
parse is left exactly as it was, and its errors are printed.

### In an editor

`frame-dsl --lsp` is a language server: errors and warnings as you type, formatting, highlighting
that comes from the compiler's own lexer rather than from a copy of its word lists, and hover, go to
definition and completion -- hovering a field says where it lands in the packet -- and quick fixes for
the mistakes with one obvious repair: a misspelt name, a missing `;`, an unused declaration. Emacs, Vim, Neovim
and VS Code each get highlighting, comments and indentation on their own, and a client for the server
on top -- Emacs needs nothing configured beyond loading
[`editors/emacs/frame-dsl-mode.el`](editors/emacs/frame-dsl-mode.el), Neovim 0.11+ beyond
`vim.lsp.enable('frame_dsl')` once `editors/vim` is on the runtime path.
[`manual/editors.md`](manual/editors.md) says what the server does,
[`editors/README.md`](editors/README.md) how to set each editor up, and
[`docs/editors.md`](docs/editors.md) why it is a server rather than a tree-sitter grammar.

## From Python

`python/framedsl` is a small package over the C library, for scripts and quick experiments -- building
a command to send by hand, or picking apart a capture. It calls `libframedsl.so` through `ctypes`, so it
encodes and decodes exactly as a C program does: there is no second implementation to disagree.

```python
import framedsl

definition = framedsl.load("examples/uart_device.fdsl")    # LoadError, with every diagnostic, if not
frame = definition.frame()
frame["TC_WRITE.size"] = 2                                  # a scalar is an int
frame["TC_WRITE.data"] = b"\xAA\xBB"                        # anything wider is bytes
wire = frame.encode("TC_WRITE")                             # b'\x03\x02\xaa\xbb\x00\x00'

reply = definition.frame()
reply.decode(wire)                                          # 'TC_WRITE'
reply["TC_WRITE.size"]                                      # 2
```

`frame.eng["PKT.field"]` reads and writes a calibrated field in engineering units, and
`frame.names["PKT.field"]` a field typed by an enum by its names -- `UnlistedError` for a value the enum
does not list, which `frame[...]` still reads as a number. A conditional field the frame does not hold
raises `AbsentError`, and `frame.present("PKT.field")` asks first. A repetition's
elements are `frame["TM_SAMPLES.samples[3].reading"]`, `frame.count("TM_SAMPLES.samples")`, or all of one
member at once with `frame.elements("TM_SAMPLES.samples.reading")`. `frame.decode_prefix(data)` returns
the packet and how many bytes it took, for walking a stream. Each error the library reports is its own
exception under `framedsl.Error`: `FieldError`, `RangeError`, `UnrecognisedError`, `TruncatedError`, ...

To install it into an environment, from the checkout (nothing is published, and nothing is downloaded --
the package builds with the standard library alone):

```sh
make python              # build the library and copy it into python/framedsl/
pip install ./python
```

Without that copy, the package looks for the library in `FRAMEDSL_LIBRARY` first, then on the system
loader's path, where `make install` puts it. To use it straight from the checkout instead:
`FRAMEDSL_LIBRARY=build/libframedsl.so.0 PYTHONPATH=python python3`.

[`examples/uart_device.py`](examples/uart_device.py) is [`examples/uart_device.c`](examples/uart_device.c)
again, same definition and same protocol, from here instead: `make python && PYTHONPATH=python
examples/uart_device.py`.

## Building

Each [release](http://100.125.208.18:3000/nico/Frame-DSL/releases) attaches a prebuilt archive for 64-bit
Linux -- the tool, the library and its header, the shell completions and every editor's support -- so
building is only needed for anything else, or to work on this.

Requires a C11 compiler, `make` and `tar`. **No network, and no credentials.**

```sh
make        # build build/frame-dsl
make test   # build and run the test suite
make dist   # the release archive, build/frame-dsl-<version>-linux-<arch>.tar.gz
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

| Target                  |                                                                                                                   |
|-------------------------|-------------------------------------------------------------------------------------------------------------------|
| `make`                  | build `build/frame-dsl`                                                                                           |
| `make example`          | build `build/uart_device`, the worked example                                                                     |
| `make lib`              | build `build/libframedsl.a` and `build/libframedsl.so` to compile your own against                                |
| `make python`           | copy the shared library into `python/framedsl/`, for `pip install ./python`                                       |
| `make deps`             | unpack the vendored CLibs libraries into `build/deps/`                                                            |
| `make test`             | run both levels of tests                                                                                          |
| `make test-unit`        | unit tests only                                                                                                   |
| `make test-system`      | system tests only                                                                                                 |
| `make test-editors`     | the Emacs, Vim and Neovim tests; each skipped when that editor is absent                                          |
| `make bench`            | time a dozen operations once each, in seconds                                                                     |
| `make baseline`         | the performance baseline over generated definitions, ~7.5 min -- see [`docs/performance.md`](docs/performance.md) |
| `make baseline-quick`   | the same, reduced to seconds                                                                                      |
| `make test-python`      | the Python bindings' tests; skipped without `python3`                                                             |
| `make vsix`             | build `build/frame-dsl-<version>.vsix`, the VS Code extension                                                     |
| `make format`           | reformat every first-party C source in place                                                                      |
| `make format-check`     | check the formatting without changing anything                                                                    |
| `make compile-commands` | regenerate `compile_commands.json` alone                                                                          |
| `make clean`            | remove `build/` and `compile_commands.json`                                                                       |

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
| `editors/`           | editor support: an Emacs major mode, a Vim/Neovim runtime directory, a VS Code extension |
| `python/`            | the Python bindings: `framedsl/` over the shared library, and their tests          |
| `completions/`       | Bash and zsh completion for `frame-dsl`, and the test that drives them without a terminal |
| `scripts/`           | `vendor_clibs.sh` and `vendor_vscode_languageclient.sh`, which update committed dependencies |
| `.forgejo/`          | CI, and the issue/PR templates `GUIDELINES.md` requires following                  |
| `third_party/clibs/` | the CLibs release tarballs, committed and unpacked at build time                   |

## Working on this repository

Read [`GUIDELINES.md`](GUIDELINES.md) first. It is short, and it is binding rather than advisory: it
covers branch and PR naming, the requirement that every PR reference an issue, the `state/*` labels
that gate who may act on an issue, and the rule against self-attribution anywhere in the repository.

[`CHANGELOG.md`](CHANGELOG.md) records what has been released, and a tag's release notes are its
section. A change on its way in writes its entry as a file of its own under
[`changelog.d/`](changelog.d/README.md), so two pull requests never edit the same lines; the entries are
folded into `CHANGELOG.md` when a version is cut.
