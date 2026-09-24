# Editor support

`.fdsl` files in an editor.

Most of what an editor shows comes from **`frame-dsl --lsp`**, the language server: diagnostics,
formatting and highlighting, all of it the compiler's own answers. What is written per editor is the
small part no server provides -- which files are `.fdsl`, what comments look like, how TAB indents --
plus regular-expression (or, for VS Code, TextMate grammar) highlighting for the moment before the
server answers. [`manual/editors.md`](../manual/editors.md) says what the server does and
[`docs/editors.md`](../docs/editors.md) why it is built this way -- including how the regular
expressions and the TextMate grammar are kept from drifting away from the compiler's own word lists,
in [`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c).

One directory per editor, each with its own README for installing it, configuring it and running its
half of `make test-editors`:

- [`emacs/`](emacs/README.md)
- [`vim/`](vim/README.md) -- also Neovim
- [`vscode/`](vscode/README.md)

## Getting `frame-dsl` onto the PATH

Every integration below finds the tool the same way: a name on `$PATH`.

```sh
make install PREFIX=~/.local     # puts ~/.local/bin/frame-dsl
```

Make sure `PREFIX/bin` (`~/.local/bin` above) is on your `PATH`. Each editor's README also says how
to point it at a build directly instead, for a checkout that has not been installed anywhere.

## Running the tests

```sh
make test-editors        # from the repository root; part of `make test`
```

Each editor's half **skips**, loudly, when that editor is not installed on the machine running the
suite -- this project builds and tests with a C compiler, `make` and `tar`, and a highlighter is not a
reason to add a fourth thing everybody has to install. What the skips give up is covered without any
editor at all by [`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c), which runs
everywhere, including in CI.
