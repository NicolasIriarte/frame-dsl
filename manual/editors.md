# Using an editor

Errors and warnings as you type, formatting on request, and highlighting that comes from the compiler
rather than from a copy of its word lists. All of it is one program -- `frame-dsl --lsp` -- speaking
the [Language Server Protocol](https://microsoft.github.io/language-server-protocol/) to whatever
editor you use.

[`editors/README.md`](../editors/README.md) has the configuration for each editor.
[`docs/editors.md`](../docs/editors.md) says why it is built this way.

## Getting the tool

```sh
make install PREFIX=~/.local      # ~/.local/bin/frame-dsl
```

Every editor integration starts by finding `frame-dsl` on the `PATH`. Nothing else has to be
installed: no runtime, no package, no network.

## What the server does

| Request | What you see |
| --- | --- |
| `textDocument/publishDiagnostics` | every error and warning the loader finds, on the right span, with its check number as the diagnostic's `code` |
| `textDocument/formatting` | the file in the one [layout](language.md#layout), as one edit |
| `textDocument/semanticTokens/full` | every name coloured as what it is: a packet, a block, a field, a calibration, a namespace, an attribute or its value |

A problem inside an imported file is shown on the `import` line that brought it in, with the real file
and position in the message and in the diagnostic's `relatedInformation` -- so "go to the problem"
lands in the right file.

What is checked is the buffer, not the file on disk. Imports resolve from the directory the buffer
belongs to, so a file being edited before it is saved behaves as it will once saved.

Hover, go to definition, completion and quick fixes are not in this version. They are
[#72](http://100.125.208.18:3000/nico/Frame-DSL/issues/72) and
[#73](http://100.125.208.18:3000/nico/Frame-DSL/issues/73).

## Without a server

Everything the server reports is available from the command line, which is what an editor without an
LSP client can use:

```sh
frame-dsl defs.fdsl --check              # one diagnostic per line, gcc-shaped
frame-dsl - --check --stdin-name defs.fdsl < buffer
frame-dsl defs.fdsl --format             # rewrite in the standard layout
frame-dsl - --format < buffer            # ... or filter a buffer through it
```

## Protocol details worth knowing

- **Transport**: JSON-RPC 2.0 over stdin and stdout, `Content-Length` framed. Nothing but protocol is
  ever written to stdout; anything else goes to stderr.
- **Synchronisation**: full text on every change. Definition files are small, and incremental sync
  would be bookkeeping with nothing to buy. A client that sends a range anyway is taken at its word.
- **Positions**: `utf-8` when the editor offers it in `general.positionEncodings`, otherwise `utf-16`,
  which is the protocol's default. It matters for any line with a non-ASCII character in it -- a
  comment, usually.
- **Lifecycle**: `initialize`, `initialized`, `shutdown`, `exit`. Exiting after `shutdown` is status 0
  and exiting without it is status 1, as the protocol says. A request before `initialize` is answered
  `-32002`, an unknown method `-32601`, and text that is not JSON `-32700`.
- **Unsaved buffers**: a document whose URI is not a `file:` URI is checked on its own -- it has no
  directory, so it cannot resolve imports.
