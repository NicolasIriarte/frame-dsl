# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/spec/v2.0.0.html).

New entries are not written here: each is a file of its own under `changelog.d/`, so that two pull
requests never edit the same lines (see `changelog.d/README.md`). Cutting a release folds them, and
anything still under `[Unreleased]`, into the version's section with `make changelog VERSION=vX.Y.Z`,
and opens a fresh `[Unreleased]` above it. A tag whose version has nothing written under it fails the
release rather than publishing an empty page. See `.forgejo/issue_template/tag.md`.

## [Unreleased]

## [0.2.0] - 2026-10-31

Conditions can now combine comparisons, encoding and decoding are faster almost everywhere, and the
library and the tool say plainly what happened. An `@if` takes `&&`, `||`, orderings and bit masks. A
performance round made 474 of the 483 points measured faster, and none slower. A decoded frame's answers
now agree with what arrived, and the tool keeps results and problems apart.

**Two changes can affect something that already works.** Writing a field with a fixed value used to
return `FRAMEDSL_OK` and be ignored, and now returns `FRAMEDSL_ERROR_FIXED`, so a program that wrote one,
even with the value it already has, has to stop. And a failed decode now goes to stderr, so a script
reading it from stdout, or matching `[FAIL]` or `[INFO]`, needs updating. No function's signature changed,
and the library is still `libframedsl.so.0`.

### Added

- **Conditions that combine comparisons.** An `@if` takes `&&`, `||` and parentheses, the orderings
  `<`, `<=`, `>` and `>=`, and a mask to test bits: `[ @if: valof(flags) & 0x4 != 0 && valof(version)
  >= 2; ]`. `&` binds tighter than the comparison, unlike C's, and `&&` tighter than `||`. A field typed
  by an enum is compared only with `==` and `!=` (check 43, new), and a mask with bits the field does
  not have, or an equality the mask alone decides, is check 8. Check 42 compares conditions by their
  `&&` terms as written. `examples/combined_conditions.fdsl` shows each.

### Changed

- **Repetitions encode and read much faster.** An element's members are read and written through the
  same routines as ordinary fields instead of a bit at a time, and its fixed values are stamped as a
  few masked bytes worked out when the definition loads: 32 three-byte elements with a fixed marker
  encode about 5x faster, and reading one element's field (`framedsl_element()` and
  `framedsl_get_u64()`) costs about a quarter of what it did. The bytes on the wire do not change.
- **A bit string off a byte boundary is copied eight bytes at a time**, in both directions, instead of a
  byte at a time: a 4 KiB one encodes about 6x and decodes about 6x faster. The bytes on the wire do
  not change.
- **Encoding and decoding cost less per field.** The fields at the start of a packet whose place and
  width the definition fixes -- every field, in most packets -- are worked out when it loads and moved
  without any per-field checks, and a decode of a packet laid out entirely that way no longer puts the
  previous frame's values aside first. A packet of 255 16-bit fields encodes and decodes in less than
  half the time it did. The bytes on the wire do not change.
- **Finding the packet no longer costs more the further down the file it is.** An encode looks the name
  up in a hash built when the definition loads, and a decode looks the frame up by the fixed bits every
  packet shares -- usually an id or a type -- then matches only the packets that agree, rather than
  trying each in turn: with 512 packets the last one now encodes and decodes in about what the first
  does, 40 times faster than before. Which packet a frame is does not change.
- **`lsb_first` scalars are reversed and written like `msb_first` ones**, a byte at a time instead of a
  bit at a time: eight 16-bit `lsb_first` fields encode about 2.3x and decode about 1.5x faster. The
  bytes on the wire do not change.
- **Decode time no longer depends on where the stack happens to sit.** A decode cleared a 2 KB array on
  the stack on every frame, and stores there could collide with the frame's loads; one packet's decode
  varied by up to 30% from one process to the next. It now varies by about 7%. `make layout` measures it.
- **CLibs moves to v0.3.0.** Its logger now writes `log_fail` and `log_halt` to stderr, and its test
  framework keeps a case's stderr with the rest of that case's output.

### Fixed

- **Fields under the same conditions nested in a different order count as one conditional part** for
  check 13, as the conditions say they are: `[ @if: a; ]` around `[ @if: b; ]` and the other way round
  used to be held to whole bytes separately.
- **Writing a fixed field is refused rather than silently ignored.** `framedsl_set_u64()`,
  `framedsl_set_name()` and `framedsl_set_eng()` used to return `FRAMEDSL_OK` for a field with a fixed
  value, and the frame went out with the definition's value anyway. They now return the new
  `FRAMEDSL_ERROR_FIXED`, for every write, the value the field already has included, and Python raises
  `FixedError`. A fixed field also reads its fixed value on a new or reset frame, where it read zero.
  The bytes of an encoded frame do not change. A program that writes a fixed field now gets an error,
  so a loop copying fields should check `FramedslField.has_fixed`.
- **`framedsl_present()` and `framedsl_count()` agree with a decoded frame.** When a fixed field past
  the ones that identify a packet differed from the definition, the decode followed the wire and these
  two answered from the definition: a field the frame did not carry was reported present, and read as a
  zero nobody sent, and a repetition reported elements it did not hold. A decoded frame now answers from
  what arrived. A frame being built still answers from the definition's value, and a conditional fixed
  field a decode did not carry reads its fixed value once its condition holds, where it read zero.
- **The tool sends results to stdout and problems to stderr.** A failed decode used to go to stdout with
  colour codes, even when piped, and a position in this project's source. A problem now goes to stderr
  as `frame-dsl: at byte 0: ...`, and a problem with a definition prints the same lines in every mode as
  `--check` does. On a terminal that name and `error:` are red and `warning:` is yellow; nothing is
  coloured in a pipe, with `NO_COLOR` set, or in a result, and describing a file prints plain text. Exit
  statuses do not change.

## [0.1.0] - 2026-10-02

The first release. Frame-DSL describes the layout of binary frames -- bit-wide fields, fixed values
that identify a packet, lengths taken from other fields, byte and bit order, shared headers, repeated
and optional parts, enumerations and calibration curves -- in a definition file a program loads at run
time, and encodes and decodes frames against it without allocating once a frame exists. It is a C
library with Python bindings, a command-line tool that checks, formats, describes and decodes, and a
language server with support for Emacs, Vim, Neovim and VS Code, all in one archive for 64-bit Linux.
There is no earlier version to compare against, so what follows is what this one holds; how it got here
is in the history of `main`, up to #127. The C API and its ABI may still change before 1.0.

### Added

- **The language.** A definition file declares packets of fields of any width in bits, with fixed
  values that identify each packet, widths computed from an earlier field's value
  (`data[128]: valof(len) * 8;`, the bracket reserving its room in bytes), and `@bit_order` and
  `@byte_order` at file, packet, block and field scope. It is checked when it loads -- checks 1 to 42
  in [`docs/design.md`](docs/design.md), three of them warnings -- so a mistake is a message at startup
  rather than a wrong frame on a live link: every packet identifiable and no two indistinguishable,
  every packet a whole number of bytes, every variable width bounded. See
  [`manual/language.md`](manual/language.md).
- **Reuse.** A named `block` is a group of fields used as an instance, `hdr: Header { id = 0x02; };`,
  whose overrides fix values per use, or pasted in without a name, `Header;`. `import "lib.fdsl";` and
  `import "lib.fdsl" as vendor;` bring another file's blocks, calibrations and enums, checked in their
  own file and reported there.
- **Repetition.** `points: Sample[2];` lays a block out twice in a row, and
  `samples[48]: Sample[valof(count)];` as many times as a field above says, within the room reserved.
  An element's fields are `TM.samples[2].reading`.
- **Conditional fields.** `[ @if: valof(sec_hdr) == 1; ]` -- or `!=` -- in front of a field, a block's
  use or an anonymous `block { }` puts it on the wire only when an earlier field has that value. Groups
  nest, and `valof(hdr.flag)` reads a field of an instance. A field a frame does not carry reads as
  absent rather than as a value nobody sent. `examples/optional_fields.fdsl` is the worked example.
- **Enumerations.** `enum TxLevel: 3 { OFF; LOW; MID; HIGH; MAX = 7; };` names a field's values, and
  `level: TxLevel;` types a field by it. A fixed value or a condition is written as a name,
  `type: PktType = HK;`, and a decoded value the enum does not list is flagged as `<unlisted>` rather
  than refused.
- **Calibrations.** `cal volts(raw) = raw * 3.3 / 4095;` and `[ @cal: volts; ]` give a field an
  engineering value, read and written through the curve -- a straight line inverted automatically,
  anything else with the `inverse` it declares.
- **The runtime.** One allocation per frame, a dedicated slot for every field of every packet, and a
  bytecode the encoder and decoder walk; nothing allocates after a frame is created. The decoder works
  out which packet a buffer is from its fixed bits, reports how many bytes the frame took, and tells an
  unrecognised frame from a truncated or a corrupt one -- and a decode that fails changes nothing.
  `framedsl_encode()` lends its buffer and `framedsl_encode_into()` writes into the caller's;
  `framedsl_get_bytes()` hands back a pointer into the frame rather than copying. See
  [`manual/api.md`](manual/api.md).
- **`libframedsl`**, static and shared (`libframedsl.so.0`, exporting only the `framedsl_*` API), with
  `framedsl.h`. `--emit-c` writes every calibration and enum out as C, for a program that wants them
  compiled in.
- **The tool.** `frame-dsl <file>` describes every packet and field; `-d <hex>` decodes a capture,
  frame after frame, with engineering values, enum names and absent fields; `--check [--werror]` prints
  problems as `path:line:col: error: message [check N]`, with a `note:` for a suggested fix;
  `--format [--check]` rewrites files into the one layout; `--list-packets` lists the packets;
  `--emit-c` writes C; `--help` and `--version`. Bash and zsh completion come with it.
- **A language server**, `frame-dsl --lsp`: diagnostics as you type, quick fixes, formatting, semantic
  highlighting from the compiler's own classification, hover with each field's offset and resolved
  orders, go to definition and completion. See [`manual/editors.md`](manual/editors.md).
- **Editor support.** An Emacs major mode (eglot and Flymake configured), a Vim and Neovim runtime
  directory (filetype, syntax, indentation, `:make`, Neovim's built-in LSP client, ALE), and a VS Code
  extension -- each highlighting before the server answers, and checked against the compiler's word
  lists so the two cannot drift.
- **Python bindings.** `framedsl.load(path)`, `frame["PKT.field"]`, `frame.eng[...]`,
  `frame.names[...]`, `frame.present(...)`, `frame.encode(packet)` and `frame.decode(data)`, over
  `ctypes` and the shared library, with an exception per kind of failure.
- **One archive per release**, `frame-dsl-<version>-linux-x86_64.tar.gz`: the tool, the library and its
  header, the completions, and every editor's support under `share/frame-dsl/editors/` -- the `.vsix`
  among them -- so editor support arrives with the tool it runs. `INSTALL.md` in it says where each
  piece goes. `make dist` builds it from a checkout, and `make test-dist` unpacks it elsewhere and uses
  it.
- **Building** needs a C11 compiler, `make` and `tar`, and no network: the libraries it builds on are
  vendored. `make test` runs unit and system tests, the editors' own tests, the Python tests and the
  saved fuzz inputs; `make fuzz` fuzzes the loader, the decoder, the formatter and the language
  server's JSON with libFuzzer, and `make baseline` measures encode and decode.
