# Vim and Neovim

One runtime directory, `editors/vim/`, for both -- Neovim reads the same `ftdetect`, `syntax`,
`indent`, `ftplugin` and `compiler` files a plain Vim install does, and adds its own built-in
language client on top. See [`../README.md`](../README.md) for what the language server gives every
editor and [`manual/editors.md`](../../manual/editors.md) for what it answers.

| File | |
| --- | --- |
| [`ftdetect/fdsl.vim`](ftdetect/fdsl.vim) | recognises a `.fdsl` file |
| [`ftplugin/fdsl.vim`](ftplugin/fdsl.vim) | comments, indent width, `gq`, `:make` |
| [`syntax/fdsl.vim`](syntax/fdsl.vim) | regular-expression highlighting |
| [`indent/fdsl.vim`](indent/fdsl.vim) | `=` reindents like the formatter |
| [`compiler/fdsl.vim`](compiler/fdsl.vim) | `:make` fills the quickfix list from `--check` |
| [`lsp/frame_dsl.lua`](lsp/frame_dsl.lua) | Neovim 0.11+'s built-in client |
| [`ale_linters/fdsl/frame_dsl.vim`](ale_linters/fdsl/frame_dsl.vim) | an [ALE](https://github.com/dense-analysis/ale) linter, for plain Vim |
| [`autoload/ale/fixers/frame_dsl.vim`](autoload/ale/fixers/frame_dsl.vim) | and an ALE fixer |
| [`frame-dsl-tests.vim`](frame-dsl-tests.vim) | tests for all of the above, run by `make test-editors` |

## Installing

```vim
set runtimepath+=~/src/frame-dsl/editors/vim
```

in `~/.vimrc` or `init.vim`, or the equivalent for whatever plugin manager you use (it is a plain
runtime directory, so `Plug '~/src/frame-dsl/editors/vim'`, `packadd`, and the rest all work). Neovim
and Vim read it identically; `filetype plugin indent on` (or Neovim's default) has to be on for any of
it to take effect.

## What it does

- **Filetype, comments, indentation.** The same as Emacs: `.fdsl` is recognised on its own, `#` is the
  comment leader, and two spaces a level -- opened by `{` or `[`, closed by `}` or `]` -- match what
  the formatter writes. `gq` (`formatprg`) runs `frame-dsl - --format`.
- **Highlighting.** `syntax/fdsl.vim` is the same regular-expression layer
  [`emacs/frame-dsl-mode.el`](../emacs/frame-dsl-mode.el) is: it knows the shape of the language, not
  its meaning, and an unknown attribute is painted as an error before anything compiles. It is what a
  buffer looks like before a language client answers.
- **`:make`.** `compiler/fdsl.vim` sets `makeprg` to `frame-dsl --check %` and an `errorformat` that
  turns what it prints into the quickfix list -- `:make`, then `:copen` or `:cnext`. It sets its
  options globally rather than with the usual per-buffer `:compiler`, which is called out in the file:
  current Vim and Neovim both mis-parse the error-type letter when `'errorformat'` is buffer-local.

## Neovim's built-in client

Neovim 0.11 and later ship their own LSP client; `lsp/frame_dsl.lua` is the config it reads, found on
the runtime path added above:

```lua
vim.lsp.enable('frame_dsl')
```

That is the whole configuration -- no further setup, the way `M-x eglot` needs none in Emacs. What it
gives you is in [`manual/editors.md`](../../manual/editors.md). The command is `frame-dsl`, found on
`$PATH` -- see [Getting `frame-dsl` onto the PATH](../README.md#getting-frame-dsl-onto-the-path).

## Vim 9

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
gets; see [`manual/editors.md`](../../manual/editors.md) for what it answers.

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

## Running the tests

```sh
make test-editors        # from the repository root; part of `make test`
```

or by hand:

```sh
vim  -u NONE -N -es --cmd 'set rtp^=editors/vim' -S editors/vim/frame-dsl-tests.vim
nvim -u NONE -N -es --cmd 'set rtp^=editors/vim' -S editors/vim/frame-dsl-tests.vim
```

`frame-dsl` needs to be on `$PATH` first; `make test-editors` arranges that itself. Each editor's half
skips, loudly, when that editor is not installed -- the same as the Emacs tests. Neovim's half
additionally starts a real `frame-dsl --lsp` through `lsp/frame_dsl.lua` and checks a diagnostic comes
back -- on Neovim 0.8 or newer, since that is where `vim.lsp.start` appeared; an older one skips that
test and says so. Plain Vim has no built-in client to test that way, which is why vim-lsp and ALE are
configured rather than shipped and tested here. The half that needs no editor at all --
`syntax/fdsl.vim`'s word lists agreeing with `src/lexer.c` and `src/parser.c` -- is
[`test/unit/9_editor_support.c`](../../test/unit/9_editor_support.c), same as for Emacs.
