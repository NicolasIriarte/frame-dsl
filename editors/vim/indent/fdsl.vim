" Two spaces a level, opened by `{' or `[' and closed by `}' or `]' -- which is every nesting the
" language has: packets, blocks and multi-line attribute groups.
if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=FrameDslIndent()
setlocal indentkeys=0{,0},0[,0],!^F,o,O

if exists("*FrameDslIndent")
  finish
endif

function! FrameDslIndent() abort
  let l:previous = prevnonblank(v:lnum - 1)
  if l:previous == 0
    return 0
  endif

  " What is on the previous line, with any comment taken off: a `#' is a comment wherever it is, so
  " a brace inside one opens nothing.
  let l:above = substitute(getline(l:previous), '#.*$', '', '')
  let l:line = substitute(getline(v:lnum), '#.*$', '', '')

  let l:indent = indent(l:previous)
  if l:above =~ '[{[]\s*$'
    let l:indent += shiftwidth()
  endif
  if l:line =~ '^\s*[}\]]'
    let l:indent -= shiftwidth()
  endif
  return l:indent < 0 ? 0 : l:indent
endfunction
