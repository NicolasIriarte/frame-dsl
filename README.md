# Frame-DSL

A small language for describing the layout of binary packets, and a C runtime that encodes and
decodes them from that description at run time.

The idea is that the shape of a frame -- its fields, their widths in bits, which values identify it,
which field gives another field's length -- is data, not code. A program links the runtime once,
loads a definition file at startup, and addresses fields by name:

```c
framedsl_set_u32(ctx, "TM_DATAGRAM_02.len", 2);
framedsl_set_arr(ctx, "TM_DATAGRAM_02.data", data, sizeof(data));
```

When a frame changes, the definition file changes. The program does not get rebuilt.

> **Status: early.** The language is being specified now, and the runtime does not exist yet. What
> builds today is a skeleton -- a binary that logs a line, and a test runner with one placeholder
> story in it. See [issue tracker](http://100.125.208.18:3000/nico/Frame-DSL/issues) for what is
> actually in flight. Nothing here is stable, and nothing is published.

## What it looks like

A definition file declares packets. Widths are in bits, everywhere:

```
pkt TM_DATAGRAM_02 {
  some_header: 8 = 0xAA;          // a fixed value: this is what identifies the packet
  len:         32;                // counts payload bytes
  data:        valof(len) * 8     // ...so the conversion to bits is written out
               max 1024;          // and bounded, so the field gets a fixed arena slot
  crc:         16;
};
```

Three things that are load-bearing rather than incidental:

- **Every width is in bits**, and `valof(x)` is the referenced field's raw value. Length fields
  rarely count exactly what follows them, so the arithmetic is written at the use site instead of
  being inferred from a convention.
- **A `valof()` field must declare a `max`.** The arena reserves the bound up front, so encoding and
  decoding never allocate, and a frame claiming more than the bound is an error rather than a
  surprise.
- **A decodable packet must carry at least one fixed-value field.** Definitions whose packets cannot
  be told apart are rejected when the file loads, not when a frame arrives on a live link.

The full language reference is in [`manual/`](manual) -- including the [C API](manual/api.md) --
and the design behind it in [`docs/`](docs).

## Building

Requires a C11 compiler, `make` and `tar`. **No network, and no credentials.**

```sh
make        # build build/frame-dsl
make test   # build and run the test suite
```

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

| Target | |
| --- | --- |
| `make` | build `build/frame-dsl` |
| `make deps` | unpack the vendored CLibs libraries into `build/deps/` |
| `make test` | run both levels of tests |
| `make test-unit` | unit tests only |
| `make test-system` | system tests only |
| `make clean` | remove `build/` |

## Layout

| Path | |
| --- | --- |
| `include/` | `framedsl.h`, the public API and the only header a user includes |
| `src/` | the runtime and the command-line front end |
| `test/` | one user story per file, `NN_name.c`; the runner registers them from the filenames |
| `manual/` | the language reference -- what someone writing a definition file needs |
| `docs/` | design documents -- why the language and runtime are shaped the way they are |
| `examples/` | example definition files, doubling as the parser's fixtures |
| `scripts/` | `vendor_clibs.sh`, which updates the committed CLibs tarballs |
| `templates/` | issue templates; `GUIDELINES.md` requires issues to follow them |
| `third_party/clibs/` | the CLibs release tarballs, committed and unpacked at build time |

## Working on this repository

Read [`GUIDELINES.md`](GUIDELINES.md) first. It is short, and it is binding rather than advisory: it
covers branch and PR naming, the requirement that every PR reference an issue, the `state/*` labels
that gate who may act on an issue, and the rule against self-attribution anywhere in the repository.

[`CHANGELOG.md`](CHANGELOG.md) records what has landed. Releases draw their notes from it.
