" Frame-DSL: the parts no language server provides -- comments, indentation and the two commands that
" put a buffer through the tool. See editors/README.md.
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

" `#' to end of line is the only comment form the language has.
setlocal commentstring=#\ %s
setlocal comments=:#

" A definition file is a column-aligned thing, and tabs in one are a nuisance to everybody reading it
" at a different tab width. Two spaces a level is what the formatter writes.
setlocal expandtab
setlocal shiftwidth=2
setlocal softtabstop=2

" gq puts the buffer through the formatter, which is the same layout `make format` writes.
setlocal formatprg=frame-dsl\ -\ --format

" :make fills the quickfix list from the compiler; see compiler/fdsl.vim for the format.
compiler fdsl

let b:undo_ftplugin = "setlocal commentstring< comments< expandtab< shiftwidth< softtabstop< formatprg<"
