# Installing a release

This archive is a prebuilt Frame-DSL for 64-bit Linux with glibc: the `frame-dsl` tool, the library and
its header, shell completions, and support for Emacs, Vim, Neovim and VS Code. Building from source is
in `README.md`.

## The tool and the library

Copy the tree into a prefix, or use it where it is unpacked:

```sh
cp -r bin lib include share ~/.local/    # then ~/.local/bin/frame-dsl
```

`bin/` has to be on your `PATH` for any editor to find `frame-dsl`. A program linking the shared library
needs `lib/` where the loader looks: run `ldconfig` after copying into a system prefix, or set
`LD_LIBRARY_PATH`. Linking `lib/libframedsl.a` statically needs neither:

```sh
cc -std=c11 -I include my_device.c lib/libframedsl.a -o my_device
```

The completions are in `share/bash-completion/completions/` and `share/zsh/site-functions/`, where
bash-completion and zsh look under a prefix they are told about.

## Editors

Every editor gets diagnostics, formatting and highlighting from `frame-dsl --lsp`, so the tool above
has to be on the `PATH` first. Each editor's README, under `share/frame-dsl/editors/`, says what it
does and how to configure it; installing it from this archive is:

- **Emacs**: `(load-file "<here>/share/frame-dsl/editors/emacs/frame-dsl-mode.el")`, or put that
  directory on `load-path`.
- **Vim and Neovim**: `set runtimepath+=<here>/share/frame-dsl/editors/vim`.
- **VS Code**: `code --install-extension <here>/share/frame-dsl/editors/vscode/frame-dsl-<version>.vsix`.

`share/frame-dsl/manual/` is the language, the C API and the editor manual.
