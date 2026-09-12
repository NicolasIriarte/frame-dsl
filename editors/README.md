# Editor support

`.fdsl` files in an editor. Emacs only, for now -- the reasoning is in
[`docs/highlighting.md`](../docs/highlighting.md), and other editors are
[#39](http://100.125.208.18:3000/nico/Frame-DSL/issues/39).

| File | |
| --- | --- |
| [`frame-dsl-mode.el`](frame-dsl-mode.el) | the major mode: font lock, comments, indentation |
| [`frame-dsl-mode-tests.el`](frame-dsl-mode-tests.el) | its tests, run by `make test-editors` |

## Installing

It is one file with no dependencies, so all three of these work. Pick whichever matches how the rest
of your configuration is written.

**`use-package`, from a clone:**

```elisp
(use-package frame-dsl-mode
  :load-path "~/src/frame-dsl/editors"
  :mode "\\.fdsl\\'")
```

**On the load path, autoloaded:**

```elisp
(add-to-list 'load-path "~/src/frame-dsl/editors")
(autoload 'frame-dsl-mode "frame-dsl-mode" "Major mode for Frame-DSL definition files." t)
(add-to-list 'auto-mode-alist '("\\.fdsl\\'" . frame-dsl-mode))
```

**Or simply:**

```elisp
(load-file "~/src/frame-dsl/editors/frame-dsl-mode.el")
```

The mode adds its own `auto-mode-alist` entry when it loads, so the explicit one above is only needed
if you want the file opened lazily rather than at startup.

## What it does

- **Font lock.** Keywords, the packet name after `pkt`, attribute names and values (only where they
  mean something -- after an `@`), the field a `valof()` names, field names and their reservations,
  numbers in all three bases, and comments.
- **An unknown attribute is coloured as a warning**, so `@byte_ordre:` looks wrong before it is
  compiled.
- **Comments.** `M-;` comments with `#`, which is the only comment syntax the language has.
- **Indentation.** Two columns per nesting level, which is what every file under `examples/` is
  written at. Packets, `block`s and multi-line attribute groups all nest.

What it does not do is understand the language: it is regular expressions, so a field named
`msb_first` is coloured as an attribute value, and a `valof()` naming a field that does not exist
looks exactly like one that does. `docs/highlighting.md` explains why that is the right trade for
now and what would change it.

## Configuring

```elisp
(setq frame-dsl-indent-offset 2)   ; the default
```

## Running the tests

```sh
make test-editors        # from the repository root; part of `make test`
```

or by hand:

```sh
emacs -Q --batch -L editors -l editors/frame-dsl-mode-tests.el -f ert-run-tests-batch-and-exit
```

`make test-editors` skips with a message when Emacs is not installed, so a machine without it can
still run the rest of the suite. The half of the mode that can be checked without Emacs -- that its
word lists still agree with `src/lexer.c` and `src/parser.c` -- is
[`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c), and that runs everywhere.

**Adding a word to the language means adding it here too.** The three `defconst` lists at the top of
`frame-dsl-mode.el` are the only place the mode writes a word down, and the C test above fails if
they and the compiler disagree. That failure is the whole reason the test exists: the alternative is
finding out months later, by staring at a word that is not coloured.
