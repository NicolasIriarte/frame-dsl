" ALE fixer for Frame-DSL: runs the buffer through 'frame-dsl - --format', the same layout `gq` and
" `make format` write. Not run unless the user opts in -- see editors/README.md -- which is ALE's
" own convention for fixers, unlike the linter beside this file.
function! ale#fixers#frame_dsl#Fix(buffer) abort
  return {
  \   'command': ale#Escape(ale#Var(a:buffer, 'fdsl_frame_dsl_executable')) . ' - --format',
  \ }
endfunction
