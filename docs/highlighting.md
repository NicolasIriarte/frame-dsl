# Syntax highlighting

Why the editor support is an Emacs major mode written by hand, what the alternatives were, and what
would have to be true to pick a different one. The mode itself is
[`editors/frame-dsl-mode.el`](../editors/frame-dsl-mode.el); how to install it is in
[`editors/README.md`](../editors/README.md).

## Three routes, not three alternatives

They look like a menu and they are not. They differ in what has to exist before a single character
changes colour, which is the thing worth comparing.

### A hand-written major mode

Regular expressions over the buffer, in Emacs Lisp. One file, no build step, no generator, and it
works in every Emacs anybody is running.

It knows the *shape* of the language and not its meaning. `valof(len)` is a keyword followed by a
paren and a name, and `len` is coloured as a name because it sits where a name sits -- not because
anything checked that the field exists. A field called `msb_first` would be coloured as an attribute
value. These are real limitations and they are also, for this language, nearly invisible: a
definition file is a few dozen lines of declarations with no nesting to speak of and no expressions
longer than `valof(len) * 8 - 32`.

The cost that is *not* invisible is a second copy of every word the language knows. That is the one
thing this route can get wrong silently, and it is why
[`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c) exists -- see below.

### Tree-sitter

A real incremental parse, and one grammar serving Emacs 29+, Neovim and Helix at once. It is the
right answer for a language with expressions worth understanding, and it would make the two
limitations above go away rather than shrink.

What it costs here:

- **A second grammar for this language**, written in JavaScript, kept in step with `src/parser.c` by
  hand. The drift problem does not go away, it moves -- from a word list to a grammar, where it is
  harder to test rather than easier.
- **A generator that is not in this repository.** `tree-sitter generate` needs `node`. The build
  rule this project holds to is that a clone builds with a C compiler, `make` and `tar`, with no
  network and no credentials; adding a toolchain that only the highlighter needs breaks that for
  everybody who never opens the file.
- **A generated C parser committed to the tree**, in a repository whose entire point is the
  hand-written one. `docs/scope.md` says writing a parser by hand *is* the project. A machine-written
  parser sitting next to it is not fatal, but it is the sort of thing to do on purpose.

None of that is an argument that tree-sitter is wrong. It is an argument that it is not worth it for
one editor, and that the moment it becomes worth it is the moment somebody is using a second one.
That is [#39](#39).

### LSP semantic tokens

The route with no second copy of anything. The compiler already knows precisely what a highlighter
wants to know: the lexer distinguishes a keyword from a name, the parser knows which identifiers are
attribute names, and the checker knows which `valof()` resolves. A server handing those out as
semantic tokens cannot drift, because there is nothing to drift from.

It is also a server, a protocol, and a client before anything is coloured -- and it is already
[#30](#30), at `horizon/eventually`. When it lands, the major mode becomes its client rather than
being thrown away: `lsp-mode` and `eglot` both attach to a major mode, and neither provides comment
syntax, indentation or `auto-mode-alist`.

## What was picked, and why in that order

The major mode, now. Tree-sitter and the other editors when somebody wants them ([#39](#39)). LSP
when [#30](#30) happens, at which point the mode is what it attaches to.

The ordering is not "cheapest first". It is that the major mode is the only one of the three that is
*not* made redundant by the others: the LSP route needs a major mode to attach to, and tree-sitter in
Emacs is a font-lock backend inside one. Whatever else arrives, `frame-dsl-mode` is still what a
`.fdsl` buffer is in.

## The drift problem, and the test that answers it

A regex highlighter carries its own copy of every word. The failure that copy causes is quiet: the
lexer grows a keyword, the mode does not, and the file somebody is looking at stops being the file
the compiler reads. Nothing fails and nothing warns. The first symptom is somebody staring at an
uncoloured word wondering whether they have mistyped it.

So the two are compared, in both directions, each by the method that suits it:

| Direction | How | Where |
| --- | --- | --- |
| the mode's words are the compiler's | run the compiler: each word the mode reserves must be refused as a field name, each attribute it knows must load | `us9_tc01`, `us9_tc02` |
| the compiler's words are the mode's | read `src/lexer.c` and `src/parser.c`, and compare the tables | `us9_tc03`, `us9_tc04` |
| the comparison is comparing something | assert the scan finds the tables, and the expected number of words in them | `us9_tc05` |

The second direction is the one that catches real drift, and it is the one that has to read the
source: there is no way to ask a built library to list its keywords, and adding one to the public API
for the benefit of a test would be the tail wagging the dog. The third exists because a scan that
silently found nothing would make every comparison above it trivially true -- which is the way this
kind of test usually rots.

The half that needs Emacs -- that the faces, the comment commands and the indentation actually work
-- is [`editors/frame-dsl-mode-tests.el`](../editors/frame-dsl-mode-tests.el), run by `make
test-editors`. It **skips**, loudly, on a machine with no Emacs: this project builds with a C
compiler, `make` and `tar`, and a highlighter is not a reason to make somebody install a fourth
thing to run the suite. Everything that can be checked without Emacs is in the C test, which runs
everywhere.

One of those Emacs cases is worth naming, because it does more than it looks like: reindenting every
file under `examples/` must change nothing. The examples are hand-written and hand-aligned, so that
is a direct assertion that the mode's idea of the layout matches what a person actually wrote.
