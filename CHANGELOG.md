# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/spec/v2.0.0.html).

Cutting a release closes out `[Unreleased]` and opens a fresh one above it; a tag whose version has
nothing written under it fails the release rather than publishing an empty page. See
`.forgejo/issue_template/tag.md`.

## [Unreleased]

### Added

- **`frame-dsl <file>... --check [--werror]`** prints a definition's problems and nothing else, one per
  line as `path:line:col: error: message [check N]` -- the shape Flymake, Vim's `errorformat`, ALE and
  a VS Code problem matcher all read. `-` reads the definition from standard input, and
  `--stdin-name <path>` checks it as that file, so an editor can check a buffer before it is saved.
- **Diagnostics have a severity and a span.** `FramedslDiagnostic` gains `severity` (error or
  warning; a warning does not fail the load) and `end_line`/`end_column`, the end of the token the
  problem is about. `framedsl_load_source()` loads a string as though it were a named file.
- **A language server**: `frame-dsl --lsp`. Diagnostics as you type (on the buffer, not the file on
  disk, with imports resolved from beside it), formatting, and semantic highlighting classified by the
  compiler's own lexer. JSON-RPC over stdio with a hand-written JSON reader and writer, fuzzed like
  the loader. `frame-dsl-mode` registers it with eglot and lsp-mode, so `M-x eglot` needs no
  configuration. `docs/highlighting.md` is now `docs/editors.md`, and `manual/editors.md` is new.
- **A formatter.** `frame-dsl <file>... --format` rewrites definition files into one layout -- two
  spaces, no tabs, fields aligned on their widths, trailing comments aligned, numbers and comments
  kept as written -- and `--format --check` reports the files that would change. It refuses a file
  that does not parse. `framedsl_format()` is the same thing through the API, and
  `frame-dsl-format-buffer` in Emacs. `make format` and `make format-check` cover `.fdsl` files, and
  every one in the repository is now in the layout.
- **Warnings**, which do not fail a load: a block, calibration or import nothing uses (check 30, only
  in a file with packets -- a file without is a library), and an attribute repeating what an enclosing
  written attribute already says (check 31). An imported file's warnings are not repeated in the
  importer. `examples/bit_order.fdsl` had two, and its block now sets something the packet does not.
- **Errors as you type in Emacs**: `frame-dsl-mode` checks the buffer through Flymake.
- `make install PREFIX=...` puts `frame-dsl` on the PATH.

- **The language**, and a compiler for it: lexer, parser, and sixteen load-time semantic checks. A
  definition file declares packets of bit-width fields, with fixed values that identify them, widths
  that depend on another field's value, and ordering attributes at file, packet, block and field
  scope. See [`manual/language.md`](manual/language.md).
- **The runtime**: an arena with a dedicated slot per field, a bytecode the encoder and decoder walk,
  and a C API that sets fields by name, encodes a named packet, and decodes a buffer into whichever
  packet its fixed bits identify -- allocating nothing after a frame is created. See
  [`manual/api.md`](manual/api.md).
- `framedsl_encode_into()`, which writes a frame into a buffer the caller already has and reports the
  size needed when it does not fit, leaving the buffer untouched.
- `framedsl_get_bytes()` hands back a pointer into the frame instead of filling a caller's buffer --
  the bytes are already there in their final form, so reading a payload needs no buffer and no copy.
  The address is fixed for the life of the frame and only that field's own writes change what is
  behind it. `framedsl_get_bytes_into()` is the copying form, for bytes that must outlive the next
  frame.
- **A decode that fails changes nothing.** The packet being walked is put aside before the first
  write and put back if the walk gives up, so a truncated or over-long frame leaves every field --
  bytes and lengths both -- holding exactly what it held before, rather than half of the frame that
  did not arrive.

- **`frame-dsl <file> -d "<hex>"`**, which decodes a capture against a definition and prints the
  fields of every frame in it. The hex may carry any separators, an odd number of digits is refused
  rather than padded, and frames back to back are walked one at a time.
- **Syntax highlighting for Emacs**: `editors/frame-dsl-mode.el`, a major mode with font lock,
  comment commands and indentation for `.fdsl` files. `docs/highlighting.md` records why it is a
  hand-written mode rather than a tree-sitter grammar or a language server, and a test compares its
  word lists against the compiler's so the two cannot drift apart unnoticed.
- A build: a `Makefile` producing `build/frame-dsl`, and a test runner built on CLibs' user-story
  test suite.
- A corpus of definition files under `test/corpus/`, driven by a table in `test/system/7_corpus.c`:
  each row names a file, the values to set and the bytes they must produce, and one loop encodes,
  compares against those bytes, decodes them into a second frame and reads every field back. Adding
  a case costs a file and a row.
- The CLibs `logger`, `bash-logger` and `testing` libraries, vendored as release tarballs under
  `third_party/clibs/` and unpacked into `build/` by the build. Building needs no network and no
  credentials. `scripts/vendor_clibs.sh` updates them, and is the only thing here that does.
- Continuous integration: every pull request and every push to `main` builds and runs the test suite.
- `README.md`, and a written scope in `docs/scope.md` stating what the project will not do as well
  as what it will.
- `CHANGELOG.md`.
