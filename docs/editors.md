# Editor support

Why `.fdsl` files are served by a language server written in C in this repository, what each editor
gets from it, and what would have to be true to pick something else. How to set an editor up is
[`editors/README.md`](../editors/README.md); what the server speaks is in
[`manual/editors.md`](../manual/editors.md).

## What an editor needs, and where each part comes from

| | Comes from | Where |
| --- | --- | --- |
| Errors and warnings as you type | the compiler, through the server | [`src/lsp.c`](../src/lsp.c) |
| Formatting | the formatter, through the server or the CLI | [`src/format.c`](../src/format.c) |
| Exact highlighting | the lexer, as semantic tokens | [`src/lsp_semantic.c`](../src/lsp_semantic.c) |
| Highlighting before the server answers | a regex mode or grammar, per editor | [`editors/`](../editors) |
| Comments, indentation, `.fdsl` recognition | a small file per editor | [`editors/`](../editors) |

Only the last two rows are written per editor, and only the fourth one duplicates anything the
compiler knows. That ordering is the whole design.

## Three routes, and why the server won

They look like a menu and they are not. They differ in what has to exist before a single character
changes colour, and in what has to be kept in step with the compiler afterwards.

### A hand-written major mode, per editor

Regular expressions over the buffer. One file per editor, no build step, and it works in whatever
version anybody is running. It knows the *shape* of the language and not its meaning: `valof(len)` is
a keyword and a paren to it, and a field named `msb_first` is coloured as an attribute value.

The cost that is not invisible is a second copy of every word the language knows, per editor. That is
the one thing this route gets wrong silently, and it is why
[`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c) exists.

It is still in use, and is meant to be: it is what a buffer looks like in the second before the server
answers, and it is what the server's client attaches to.

### Tree-sitter

A real incremental parse, and one grammar serving Emacs 29+, Neovim and Helix at once. What it costs
here:

- **A second grammar for this language**, written in JavaScript, kept in step with `src/parser.c` by
  hand. The drift problem does not go away, it moves -- from a word list to a grammar, where it is
  harder to test rather than easier.
- **A generator that is not in this repository.** `tree-sitter generate` needs `node`. The rule this
  build holds to is that a clone builds with a C compiler, `make` and `tar`, with no network and no
  credentials.
- **A generated C parser committed to the tree**, in a repository whose entire point is the
  hand-written one.

And having paid all that, it still would not know that `valof(len)` names a field that does not
exist, which is the thing people actually wanted.

### A language server

The route with no second copy of anything. The compiler already knows precisely what a highlighter
wants to know: the lexer distinguishes a keyword from a name, the parser knows which identifiers are
attribute names, and the checker knows which `valof()` resolves. A server handing those out cannot
drift, because there is nothing to drift from -- a keyword added to `src/lexer.c` is highlighted the
day it is added, in every editor at once.

It costs a server, a protocol and a client. The server is about 1,200 lines of C here, including its
own JSON reader; the protocol is JSON-RPC over stdio; and the client is a few lines of configuration
per editor, because every editor worth naming already ships one.

**That is the trade that decided it**: the per-editor cost drops to almost nothing, and the part that
is written once is written in the language this project is already written in, against the compiler
it already has.

## What is still per editor, and why

Two things, both small:

- **Base highlighting.** Semantic tokens arrive a few milliseconds after the file opens, and in some
  editors only when the server is running. A file should not be black and white until then.
- **The parts no server provides.** Which files are `.fdsl`, what `M-;` comments with, what `TAB`
  does. The protocol has nothing to say about any of it.

Both are regular expressions, and both are covered by the drift test.

## The drift problem, and the test that answers it

A regex highlighter carries its own copy of every word. The failure that copy causes is quiet: the
lexer grows a keyword, the mode does not, and the file somebody is looking at stops being the file the
compiler reads. Nothing fails and nothing warns.

So the two are compared, in both directions, each by the method that suits it:

| Direction | How | Where |
| --- | --- | --- |
| the mode's words are the compiler's | run the compiler: each word the mode reserves must be refused as a field name, each attribute it knows must load | `us9_tc01`, `us9_tc02` |
| the compiler's words are the mode's | read `src/lexer.c` and `src/parser.c`, and compare the tables | `us9_tc03`, `us9_tc04` |
| the comparison is comparing something | assert the scan finds the tables, and the expected number of words in them | `us9_tc05` |

The second direction is the one that catches real drift, and it is the one that has to read the
source: there is no way to ask a built library to list its keywords, and adding one to the public API
for the benefit of a test would be the tail wagging the dog.

**The server needs none of this.** Its classification comes from the lexer's own tokens, so the only
thing to test about it is that each kind of name is classified as what it is -- which
`test/unit/13_lsp.c` does by decoding the tokens it sends.

The half that needs Emacs -- faces, comment commands, indentation, Flymake and the eglot round trip
against the real server -- is [`editors/frame-dsl-mode-tests.el`](../editors/frame-dsl-mode-tests.el),
run by `make test-editors`. It **skips**, loudly, on a machine with no Emacs.

One of those cases is worth naming, because it does more than it looks like: reindenting every file
under `examples/` must change nothing. The examples are written by the formatter, so that is a direct
assertion that the mode's idea of the layout and the formatter's are the same one.
