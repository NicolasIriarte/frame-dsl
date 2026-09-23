# Editor support

`.fdsl` files in an editor.

Most of what an editor shows comes from **`frame-dsl --lsp`**, the language server: diagnostics,
formatting and highlighting, all of it the compiler's own answers. What is written per editor is the
small part no server provides -- which files are `.fdsl`, what comments look like, how TAB indents --
plus regular-expression highlighting for the moment before the server answers.
[`manual/editors.md`](../manual/editors.md) says what the server does and
[`docs/editors.md`](../docs/editors.md) why it is built that way. Editors other than Emacs are
[#39](http://100.125.208.18:3000/nico/Frame-DSL/issues/39).

| File | |
| --- | --- |
| [`frame-dsl-mode.el`](frame-dsl-mode.el) | the major mode: font lock, comments, indentation, Flymake |
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
looks exactly like one that does. With the language server running, that layer is only what
you see in the moment before it answers; `docs/editors.md` explains the arrangement.

- **Errors as you type.** A Flymake backend pipes the buffer to `frame-dsl - --check --stdin-name
  <file>`, so what is checked is what is on screen -- saved or not -- and imports resolve from beside
  the file. The messages are the compiler's own. A problem inside an imported file is shown on the
  `import` line that brought it in, naming the file and position it is really at. `flymake-mode` is
  switched on by itself when `frame-dsl` can be found; `M-x flymake-show-buffer-diagnostics` lists
  everything.

- **The language server.** `M-x eglot` in a `.fdsl` buffer starts `frame-dsl --lsp` and needs no
  configuration: the mode registers the server with eglot (built into Emacs 29 and later) and with
  `lsp-mode` when that is what you use. What it gives you is in
  [`manual/editors.md`](../manual/editors.md). With eglot running, its diagnostics replace the Flymake
  backend below -- the same messages, from a server that is already holding the file. To start it
  automatically:

  ```elisp
  (add-hook 'frame-dsl-mode-hook #'eglot-ensure)
  ```

- **Formatting.** `M-x frame-dsl-format-buffer` puts the buffer through `frame-dsl --format`, in the
  layout every file in this repository is in. Only what changed is replaced, so point and undo
  survive. A buffer that does not parse is left alone and the error says why. To do it on every
  save:

  ```elisp
  (add-hook 'frame-dsl-mode-hook
            (lambda () (add-hook 'before-save-hook #'frame-dsl-format-buffer nil t)))
  ```

## Getting `frame-dsl` onto the PATH

The Flymake backend runs the `frame-dsl` tool, so it has to be somewhere Emacs can find it. From the
repository root:

```sh
make install PREFIX=~/.local     # puts ~/.local/bin/frame-dsl
```

and make sure `~/.local/bin` is on `exec-path`. Or point the mode straight at a build:

```elisp
(setq frame-dsl-program "~/src/frame-dsl/build/frame-dsl")
```

## Configuring

```elisp
(setq frame-dsl-indent-offset 2)   ; the default
(setq frame-dsl-program "frame-dsl")  ; the default: a name on exec-path, or a file name
(setq frame-dsl-flymake nil)       ; don't switch flymake-mode on by itself
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
