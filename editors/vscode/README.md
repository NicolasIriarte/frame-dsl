# VS Code

| File | |
| --- | --- |
| [`package.json`](package.json) | the extension manifest |
| [`language-configuration.json`](language-configuration.json) | comments, brackets, indentation |
| [`syntaxes/fdsl.tmLanguage.json`](syntaxes/fdsl.tmLanguage.json) | TextMate grammar highlighting |
| [`extension.js`](extension.js) | starts `frame-dsl --lsp` |
| [`vendor/vscode-languageclient.js`](vendor/) | the language client, bundled -- see `NOTICE` beside it |

See [`../README.md`](../README.md) for what the language server gives every editor and
[`manual/editors.md`](../../manual/editors.md) for what it answers.

Plain JavaScript, no TypeScript and no build step for the extension itself: `extension.js` requires
the vendored client directly. The vendoring -- combining `vscode-languageclient` and its own
dependencies into that one file -- is the one place npm is needed anywhere in this repository, and it
is a script run by hand, occasionally, like `scripts/vendor_clibs.sh`:

```sh
scripts/vendor_vscode_languageclient.sh          # re-bundles the version already vendored
scripts/vendor_vscode_languageclient.sh 9.0.1    # or a specific one
```

## Installing

```sh
make vsix                 # writes build/frame-dsl-<version>.vsix, with zip alone
code --install-extension build/frame-dsl-<version>.vsix
```

No marketplace publishing: each [Forgejo release](http://100.125.208.18:3000/nico/Frame-DSL/releases)
carries the `.vsix` instead, built the same way, in its archive at
`share/frame-dsl/editors/vscode/` -- beside the `frame-dsl` it runs.

## What it does

- **Highlighting**, from the TextMate grammar -- the same word lists and the same shape as the Vim
  syntax file (an unknown attribute included), checked against it and the compiler by
  [`test/unit/9_editor_support.c`](../../test/unit/9_editor_support.c).
- **Everything else**, from the language server: diagnostics as you type, formatting
  (`Format Document`), semantic highlighting once the server answers. What it gives you is in
  [`manual/editors.md`](../../manual/editors.md). `extension.js` starts `frame-dsl --lsp` and attaches
  to every `.fdsl` buffer, saved or not, with no further setup -- there is no eglot- or lsp-mode-style
  choice to make, the way there is in Emacs.
- **A clear message when `frame-dsl` cannot be found**, rather than a silent failure to attach.

## Configuring

The `frameDsl.path` setting (`Preferences: Open Settings`) is the path to the `frame-dsl` binary, for
when it is not on `$PATH` -- see
[Getting `frame-dsl` onto the PATH](../README.md#getting-frame-dsl-onto-the-path) for the usual way to
put it there:

```json
{ "frameDsl.path": "/home/you/src/frame-dsl/build/frame-dsl" }
```

The default is the bare name `frame-dsl`, looked up on `$PATH`.

## Testing

VS Code has no headless batch mode to script the way `make test-editors` drives Emacs, Vim and
Neovim, so this half is checked by hand instead: install the `.vsix` above into a real VS Code, open a
file under `examples/`, and check highlighting, diagnostics, formatting and hover each still work,
every time this directory or `extension.js`'s side of the protocol changes. The half that needs no
editor at all -- that the TextMate grammar and `package.json` still agree with `src/lexer.c` and
`src/parser.c` -- is [`test/unit/9_editor_support.c`](../../test/unit/9_editor_support.c), and that
runs everywhere, including in CI.
