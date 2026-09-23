" `:make` on a definition file: the diagnostics `frame-dsl --check` prints, in the quickfix list.
"
" The output is the shape every compiler prints -- path:line:column: severity: message -- so the
" format below is the standard one, plus a line for a problem with no position (a file that cannot be
" read) and one for the `[check N]` note the tool puts at the end of a message.
if exists("current_compiler")
  finish
endif
let current_compiler = "fdsl"

" Global rather than the usual per-buffer `:setlocal` a bare `:compiler {name}` asks for (see
" `:help :CompilerSet`): current Vim and Neovim both mis-parse `%t` -- the letter that tells an error
" from a warning -- when 'errorformat' is buffer-local, silently leaving every item untyped. One
" definition file language in a session is the ordinary case anyway, so global costs nothing here.
command! -nargs=* CompilerSet set <args>

CompilerSet makeprg=frame-dsl\ --check\ %
CompilerSet errorformat=%f:%l:%c:\ %trror:\ %m,
                       \%f:%l:%c:\ %tarning:\ %m,
                       \%f:\ %trror:\ %m,
                       \%-G%.%#
