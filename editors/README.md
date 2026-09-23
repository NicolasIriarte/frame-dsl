# Editor support

`.fdsl` files in an editor.

Most of what an editor shows comes from **`frame-dsl --lsp`**, the language server: diagnostics,
formatting and highlighting, all of it the compiler's own answers. What is written per editor is the
small part no server provides -- which files are `.fdsl`, what comments look like, how TAB indents --
plus regular-expression (or, for VS Code, TextMate grammar) highlighting for the moment before the
server answers. [`manual/editors.md`](../manual/editors.md) says what the server does and
[`docs/editors.md`](../docs/editors.md) why it is built this way.

Three editors, three sections below: [Emacs](#emacs), [Vim and Neovim](#vim-and-neovim),
[VS Code](#vs-code).

## Emacs

| File | |
| --- | --- |
| [`frame-dsl-mode.el`](frame-dsl-mode.el) | the major mode: font lock, comments, indentation, Flymake |
| [`frame-dsl-mode-tests.el`](frame-dsl-mode-tests.el) | its tests, run by `make test-editors` |

### Installing

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

### What it does

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

### Getting `frame-dsl` onto the PATH

The Flymake backend runs the `frame-dsl` tool, so it has to be somewhere Emacs can find it. From the
repository root:

```sh
make install PREFIX=~/.local     # puts ~/.local/bin/frame-dsl
```

and make sure `~/.local/bin` is on `exec-path`. Or point the mode straight at a build:

```elisp
(setq frame-dsl-program "~/src/frame-dsl/build/frame-dsl")
```

### Configuring

```elisp
(setq frame-dsl-indent-offset 2)   ; the default
(setq frame-dsl-program "frame-dsl")  ; the default: a name on exec-path, or a file name
(setq frame-dsl-flymake nil)       ; don't switch flymake-mode on by itself
```

### Running the tests

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

## Vim and Neovim

One runtime directory, `editors/vim/`, for both -- Neovim reads the same `ftdetect`, `syntax`,
`indent`, `ftplugin` and `compiler` files a plain Vim install does, and adds its own built-in
language client on top.

| File | |
| --- | --- |
| [`ftdetect/fdsl.vim`](vim/ftdetect/fdsl.vim) | recognises a `.fdsl` file |
| [`ftplugin/fdsl.vim`](vim/ftplugin/fdsl.vim) | comments, indent width, `gq`, `:make` |
| [`syntax/fdsl.vim`](vim/syntax/fdsl.vim) | regular-expression highlighting |
| [`indent/fdsl.vim`](vim/indent/fdsl.vim) | `=` reindents like the formatter |
| [`compiler/fdsl.vim`](vim/compiler/fdsl.vim) | `:make` fills the quickfix list from `--check` |
| [`lsp/frame_dsl.lua`](vim/lsp/frame_dsl.lua) | Neovim 0.11+'s built-in client |
| [`ale_linters/fdsl/frame_dsl.vim`](vim/ale_linters/fdsl/frame_dsl.vim) | an [ALE](https://github.com/dense-analysis/ale) linter, for plain Vim |
| [`autoload/ale/fixers/frame_dsl.vim`](vim/autoload/ale/fixers/frame_dsl.vim) | and an ALE fixer |
| [`frame-dsl-tests.vim`](vim/frame-dsl-tests.vim) | tests for all of the above, run by `make test-editors` |

### Installing

```vim
set runtimepath+=~/src/frame-dsl/editors/vim
```

in `~/.vimrc` or `init.vim`, or the equivalent for whatever plugin manager you use (it is a plain
runtime directory, so `Plug '~/src/frame-dsl/editors/vim'`, `packadd`, and the rest all work). Neovim
and Vim read it identically; `filetype plugin indent on` (or Neovim's default) has to be on for any of
it to take effect.

### What it does

- **Filetype, comments, indentation.** The same as Emacs: `.fdsl` is recognised on its own, `#` is the
  comment leader, and two spaces a level -- opened by `{` or `[`, closed by `}` or `]` -- match what
  the formatter writes. `gq` (`formatprg`) runs `frame-dsl - --format`.
- **Highlighting.** `syntax/fdsl.vim` is the same regular-expression layer `frame-dsl-mode.el` is: it
  knows the shape of the language, not its meaning, and an unknown attribute is painted as an error
  before anything compiles. It is what a buffer looks like before a language client answers.
- **`:make`.** `compiler/fdsl.vim` sets `makeprg` to `frame-dsl --check %` and an `errorformat` that
  turns what it prints into the quickfix list -- `:make`, then `:copen` or `:cnext`. It sets its
  options globally rather than with the usual per-buffer `:compiler`, which is called out in the file:
  current Vim and Neovim both mis-parse the error-type letter when `'errorformat'` is buffer-local.

### Neovim's built-in client

Neovim 0.11 and later ship their own LSP client; `lsp/frame_dsl.lua` is the config it reads, found on
the runtime path added above:

```lua
vim.lsp.enable('frame_dsl')
```

That is the whole configuration -- no further setup, the way `M-x eglot` needs none in Emacs. What it
gives you is in [`manual/editors.md`](../manual/editors.md). The command is `frame-dsl`, found on
`$PATH`; `make install PREFIX=~/.local` puts it there.

### Vim 9

Vim has no built-in language client, so checking and formatting go through two smaller, separate
routes instead of one server connection.

**[vim-lsp](https://github.com/prabirshrestha/vim-lsp)**, if you already use it for other languages:

```vim
if executable('frame-dsl')
  autocmd User lsp_setup call lsp#register_server({
  \   'name': 'frame-dsl',
  \   'cmd': {server_info->['frame-dsl', '--lsp']},
  \   'allowlist': ['fdsl'],
  \ })
endif
```

This is the real server -- diagnostics, formatting, semantic tokens -- the same as Neovim's client
gets; see [`manual/editors.md`](../manual/editors.md) for what it answers.

**[ALE](https://github.com/dense-analysis/ale)**, which does not speak the protocol but does not need
to: `ale_linters/fdsl/frame_dsl.vim` and `autoload/ale/fixers/frame_dsl.vim` are found automatically
once `editors/vim` is on the runtime path, the same way the syntax and indent files are. The linter
runs `frame-dsl - --check --stdin-name <file>` on every change ALE would normally lint on -- the
buffer, not the file on disk, exactly like the Flymake backend and vim-lsp above. The fixer is not run
unless you ask ALE to, which is ALE's own convention for fixers rather than linters:

```vim
let g:ale_fixers = {'fdsl': ['frame_dsl']}
" let g:ale_fix_on_save = 1   " optional
```

vim-lsp and ALE's linter both report the same diagnostics; running both at once means seeing each one
twice, so pick whichever fits how you already use Vim. Neither is a dependency of this repository --
if it is not installed, the corresponding file has nothing to attach to and does nothing.

### Getting `frame-dsl` onto the PATH

```sh
make install PREFIX=~/.local     # puts ~/.local/bin/frame-dsl
```

Every route above -- `:make`, vim-lsp, ALE, Neovim's client -- finds `frame-dsl` the same way: a name
on `$PATH`.

### Running the tests

```sh
make test-editors        # from the repository root; part of `make test`
```

or by hand:

```sh
vim  -u NONE -N -es --cmd 'set rtp^=editors/vim' -S editors/vim/frame-dsl-tests.vim
nvim -u NONE -N -es --cmd 'set rtp^=editors/vim' -S editors/vim/frame-dsl-tests.vim
```

`frame-dsl` needs to be on `$PATH` first; `make test-editors` arranges that itself. Each editor's half
skips, loudly, when that editor is not installed -- the same as the Emacs tests above. Neovim's half
additionally starts a real `frame-dsl --lsp` through `lsp/frame_dsl.lua` and checks a diagnostic comes
back -- on Neovim 0.8 or newer, since that is where `vim.lsp.start` appeared; an older one skips that
test and says so. Plain Vim has no built-in client to test that way, which is why vim-lsp and ALE are
configured rather than shipped and tested here. The half that needs no editor at all --
`syntax/fdsl.vim`'s word lists agreeing with `src/lexer.c` and `src/parser.c` -- is
[`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c), same as for Emacs.

## VS Code

| File | |
| --- | --- |
| [`package.json`](vscode/package.json) | the extension manifest |
| [`language-configuration.json`](vscode/language-configuration.json) | comments, brackets, indentation |
| [`syntaxes/fdsl.tmLanguage.json`](vscode/syntaxes/fdsl.tmLanguage.json) | TextMate grammar highlighting |
| [`extension.js`](vscode/extension.js) | starts `frame-dsl --lsp` |
| [`vendor/vscode-languageclient.js`](vscode/vendor/) | the language client, bundled -- see `NOTICE` beside it |

Plain JavaScript, no TypeScript and no build step for the extension itself: `extension.js` requires
the vendored client directly. The vendoring -- combining `vscode-languageclient` and its own
dependencies into that one file -- is the one place npm is needed anywhere in this repository, and it
is a script run by hand, occasionally, like `scripts/vendor_clibs.sh`:

```sh
scripts/vendor_vscode_languageclient.sh          # re-bundles the version already vendored
scripts/vendor_vscode_languageclient.sh 9.0.1    # or a specific one
```

### Installing

```sh
make vsix                 # writes build/frame-dsl-<version>.vsix, with zip alone
code --install-extension build/frame-dsl-<version>.vsix
```

No marketplace publishing: a `.vsix` is attached to each [Forgejo
release](http://100.125.208.18:3000/nico/Frame-DSL/releases) instead, built the same way.

### What it does

- **Highlighting**, from the TextMate grammar -- the same word lists and the same shape as the Vim
  syntax file (an unknown attribute included), checked against it and the compiler by
  [`test/unit/9_editor_support.c`](../test/unit/9_editor_support.c).
- **Everything else**, from the language server: diagnostics as you type, formatting
  (`Format Document`), semantic highlighting once the server answers. What it gives you is in
  [`manual/editors.md`](../manual/editors.md). `extension.js` starts `frame-dsl --lsp` and attaches to
  every `.fdsl` buffer, saved or not.
- **A clear message when `frame-dsl` cannot be found**, rather than a silent failure to attach.

### Configuring

The `frameDsl.path` setting (`Preferences: Open Settings`) is the path to the `frame-dsl` binary, for
when it is not on `$PATH`:

```json
{ "frameDsl.path": "/home/you/src/frame-dsl/build/frame-dsl" }
```

The default is the bare name `frame-dsl`, looked up on `$PATH` -- `make install PREFIX=~/.local` puts
it there.
