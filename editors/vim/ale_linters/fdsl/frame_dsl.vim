" ALE linter for Frame-DSL, for plain Vim: Vim 9 has no built-in language server client, so this and
" the fixer in autoload/ale/fixers/frame_dsl.vim are what checking and formatting look like without
" one. Both run 'frame-dsl' itself -- the same binary and the same --check/--format the compiler
" plugin and every other editor use -- so this cannot say anything --check would not.
"
" Autoloaded by ALE, not required by anything else here: a machine without ALE installed never
" reaches this file, the way one without eglot never reaches frame-dsl-mode's eglot integration.
call ale#Set('fdsl_frame_dsl_executable', 'frame-dsl')

" 'defs.fdsl:4:3: error: message [check N]' and the warning form -- what --check prints, one
" diagnostic per line. Matches compiler/fdsl.vim's errorformat; see that file for why the type
" character is spelled out here rather than captured as one.
function! ale_linters#fdsl#frame_dsl#Handle(buffer, lines) abort
  let l:pattern = '^[^:]\+:\(\d\+\):\(\d\+\): \(error\|warning\): \(.\+\)$'
  let l:output = []
  for l:line in a:lines
    let l:match = matchlist(l:line, l:pattern)
    if len(l:match) == 0
      continue
    endif
    call add(l:output, {
    \   'lnum': l:match[1] + 0,
    \   'col': l:match[2] + 0,
    \   'type': l:match[3] is# 'error' ? 'E' : 'W',
    \   'text': l:match[4],
    \ })
  endfor
  return l:output
endfunction

" The buffer, not the file on disk: '-' with --stdin-name so what is linted is what is on screen,
" and imports still resolve from beside the real file. The full path, the way frame-dsl-mode's
" Flymake backend passes it, so a relative cwd cannot change where an import resolves from.
function! ale_linters#fdsl#frame_dsl#GetCommand(buffer) abort
  return ale#Escape(ale#Var(a:buffer, 'fdsl_frame_dsl_executable'))
  \   . ' - --check --stdin-name ' . ale#Escape(fnamemodify(bufname(a:buffer), ':p'))
endfunction

call ale#linter#Define('fdsl', {
\   'name': 'frame_dsl',
\   'executable': {b -> ale#Var(b, 'fdsl_frame_dsl_executable')},
\   'command': function('ale_linters#fdsl#frame_dsl#GetCommand'),
\   'callback': 'ale_linters#fdsl#frame_dsl#Handle',
\   'read_buffer': 1,
\ })
