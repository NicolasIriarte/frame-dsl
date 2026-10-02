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
