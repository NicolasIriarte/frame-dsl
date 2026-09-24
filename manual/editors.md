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
| `textDocument/hover` | what the name under the cursor is -- see [Hover](#hover) |
| `textDocument/definition` | where it is declared, in whichever file that is |
| `textDocument/completion` | what can be written where the cursor is -- see [Completion](#completion) |
| `textDocument/codeAction` | the one obvious fix for a diagnostic under the cursor -- see [Quick fixes](#quick-fixes) |

A problem inside an imported file is shown on the `import` line that brought it in, with the real file
and position in the message and in the diagnostic's `relatedInformation` -- so "go to the problem"
lands in the right file.

What is checked is the buffer, not the file on disk. Imports resolve from the directory the buffer
belongs to, so a file being edited before it is saved behaves as it will once saved.

### Hover

- **A field in a packet**: its width, its offset in the packet (bit, and byte when it is on one), its
  bit and byte order once every attribute scope has been applied, its fixed value and its
  calibration. These are the facts hardest to work out by reading, and they come from the same layout
  the runtime uses -- so they need the file to load. While it has errors the hover says what the
  declaration alone says, and that the offset is known once it loads.
- **A field inside an override**, `hdr: Header { id = 2; }`: the same, for `hdr.id` in this packet.
- **An instance**, `hdr` or the `axes` of `axes.z`: the block it is, and the bits it covers.
- **A field declared in a block**: its width and orders. It has no offset until a packet uses it.
- **A `valof()` target**: the field it names.
- **A block** or **a calibration**, used or declared: its fields, or its line of source, and the file
  it came from when it was imported.
- **A namespace**, `link` in `link.Header`: the import it stands for and what that brings.
- **A packet's name**: how many fields it has, and its length when that is fixed.
- **An attribute's name**: what it does and the values it takes.

Go to definition follows the same names: a `valof()` target, a block or calibration however it is
reached -- directly, through a flat import or through an `as` namespace -- a field in an override,
and a namespace, which goes to its `import`.

### Completion

| After | Offers |
| --- | --- |
| `@` | the attribute names: `bit_order`, `byte_order`, `cal` |
| `@bit_order:`, `@byte_order:` | that attribute's values |
| `@cal:` | the calibrations, local and imported |
| `valof(` | the scalar fields declared above, in the same packet or block |
| `ns.` | what that import brings: its calibrations after `@cal:`, its blocks anywhere else |
| `name:` | the blocks, and `valof` |
| the start of a declaration in a body | the blocks, to paste one in, and `block` |
| `Block {` of a use | that block's fields, to override |
| the start of a line at file level | `pkt`, `block`, `cal`, `import` |

### Quick fixes

A diagnostic carries a fix when there is exactly one obvious way to repair it, and the editor offers it
as a quick fix (VS Code's light bulb, `vim.lsp.buf.code_action()`, Eglot's `eglot-code-actions`):

| Diagnostic | Fix |
| --- | --- |
| an unknown attribute name or value | **did you mean** the closest one |
| a `valof()` naming no field | **did you mean** the closest field declared above it |
| an unknown block, or calibration after `@cal:` or in a curve | **did you mean** the closest one, imported names included as this file writes them (`link.Header`) |
| a missing `;` | **insert `;`** straight after what came before it |
| an unused block, calibration or import (check 30) | **remove** it, with its attribute group and one of the blank lines around it |
| a redundant attribute (check 31) | **remove** it, or its whole group when it is the only thing in it |

"Closest" means within two edits -- a letter added, dropped, changed, or two neighbours swapped -- and
fewer edits than the name has letters. When two names are equally close nothing is offered: a guess
between them would be a coin toss.

A removal is not offered while the file has errors: a use with a typo in it -- `Heder` for `Header`
-- is exactly what makes the block it meant look unused. A problem in an imported file carries no fix
here; open that file to fix it.

The fixes are made by the checks, not by the server, so `--check` prints the same suggestion as a
`note:` line under its diagnostic:

```text
defs.fdsl:1:2: error: unknown attribute 'byte_ordre'; this version knows 'bit_order', 'byte_order' and 'cal'
defs.fdsl:1:2: note: did you mean 'byte_order'?
```

Its position is where the fix would edit. The note lines match neither Vim's `errorformat` nor the
Emacs and ALE patterns, so those show the diagnostic alone. A program using the library reads the same
fix from `FramedslDiagnostic.fix_title`, `fix_text` and the `fix_*` positions.

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
