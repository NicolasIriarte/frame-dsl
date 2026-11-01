" Tests for the Vim runtime files. Run from the repository root by `make test-editors`:
"
"     vim -u NONE -N -es --cmd 'set rtp^=editors/vim' -S editors/vim/frame-dsl-tests.vim
"
" Neovim runs the same file, and additionally attaches lsp/frame_dsl.lua to a real 'frame-dsl --lsp'
" -- there is no equivalent in plain Vim, which has no built-in client of its own. Both skip -- and
" `make test-editors` says so -- when the editor is not installed, for the same reason the Emacs
" tests do: this project builds and tests with a C compiler, make and tar. The part that can be
" checked without any editor at all is the word-list comparison in test/unit/9_editor_support.c.
" `frame-dsl` itself must be on PATH; `make test-editors` arranges that.
set nocompatible
filetype plugin indent on
syntax on

let s:sample = [
      \ 'import "common/link.fdsl" as ns;',
      \ '@byte_order: little_endian;',
      \ '@byte_ordre: little_endian;',
      \ 'cal volts(raw) = raw * 2 inverse (eng) = eng / 2;',
      \ 'block Header {',
      \ '  id: 8 = 0xAA;   # the id',
      \ '};',
      \ 'pkt TM {',
      \ '  hdr: ns.Header;',
      \ '  [ @cal: volts; ]',
      \ '  size: 8;',
      \ '  data[64]: valof(size) * 8;',
      \ '};',
      \ ]

function! s:Open(lines) abort
  enew!
  call setline(1, a:lines)
  set filetype=fdsl
  " Fontification is lazy; asking for a group forces it.
  call synID(1, 1, 1)
endfunction

function! s:GroupAt(line, column) abort
  return synIDattr(synID(a:line, a:column, 1), 'name')
endfunction

" A .fdsl file is recognised without anybody configuring it.
function! s:TestFiletype() abort
  enew!
  execute 'edit' fnameescape(tempname() . '.fdsl')
  call assert_equal('fdsl', &filetype, 'filetype of a .fdsl file')
  call assert_equal('#\ %s', escape(&commentstring, ' '), 'commentstring')
  call assert_equal(2, &shiftwidth, 'shiftwidth')
  call assert_equal(1, &expandtab, 'expandtab')
  bwipeout!
endfunction

" Each kind of thing looks like what it is. The unknown attribute is the one worth having: a typo is
" painted as an error before anything is compiled.
function! s:TestSyntax() abort
  call s:Open(s:sample)
  let l:cases = [
        \ [1, 1, 'fdslContextual', 'import'],
        \ [1, 8, 'fdslString', 'the path'],
        \ [1, 27, 'fdslContextual', 'as'],
        \ [1, 30, 'fdslNamespace', 'the namespace'],
        \ [2, 2, 'fdslAttributeName', 'a known attribute'],
        \ [2, 14, 'fdslAttributeValue', 'a known value'],
        \ [3, 2, 'fdslAttributeUnknown', 'an unknown attribute'],
        \ [4, 1, 'fdslContextual', 'cal'],
        \ [4, 5, 'fdslCalDecl', 'the curve name'],
        \ [4, 26, 'fdslContextual', 'inverse'],
        \ [5, 1, 'fdslKeyword', 'block'],
        \ [5, 7, 'fdslPacketName', 'the block name'],
        \ [6, 3, 'fdslFieldName', 'a field name'],
        \ [6, 11, 'fdslNumber', 'a hex number'],
        \ [6, 19, 'fdslComment', 'a comment'],
        \ [8, 1, 'fdslKeyword', 'pkt'],
        \ [8, 5, 'fdslPacketName', 'the packet name'],
        \ [9, 8, 'fdslTypeName', 'a block used as a type'],
        \ [10, 6, 'fdslAttributeName', 'cal as an attribute'],
        \ [10, 11, 'fdslCalName', 'the curve it names'],
        \ [12, 13, 'fdslKeyword', 'valof'],
        \ [12, 19, 'fdslValofTarget', 'what valof names'],
        \ ]
  for [l:line, l:column, l:group, l:what] in l:cases
    call assert_equal(l:group, s:GroupAt(l:line, l:column), l:what)
  endfor
  bwipeout!
endfunction

" A block pasted in without an instance name is a block, whether or not it has overrides; the `block {'
" of an attribute scope, the same shape, is still a keyword.
function! s:TestPastedSyntax() abort
  call s:Open(['pkt TM {', '  Header { id := 1; };', '  ns.Stamp;', '  block {', '    x: 8;', '  };', '};'])
  call assert_equal('fdslTypeName', s:GroupAt(2, 3), 'a pasted block')
  call assert_equal('fdslTypeName', s:GroupAt(3, 6), 'a pasted block through a namespace')
  call assert_equal('fdslKeyword', s:GroupAt(4, 3), 'an attribute scope')
  bwipeout!
endfunction

" An enum's name is a type where it is declared and where a field is typed by it, with a value or
" without; `enum' is a word only where it declares one, so a field may still be called that.
function! s:TestEnumSyntax() abort
  call s:Open(['enum TxLevel: 3 {', '  MAX = 7;', '};', 'pkt TM {', '  level: TxLevel := MAX;', '  enum: 8;', '};'])
  call assert_equal('fdslContextual', s:GroupAt(1, 1), 'enum')
  call assert_equal('fdslPacketName', s:GroupAt(1, 6), 'the enum name')
  call assert_equal('fdslTypeName', s:GroupAt(5, 10), 'an enum used as a type, with a value')
  call assert_equal('fdslOperator', s:GroupAt(5, 18), 'the := of a fixed value')
  call assert_equal('fdslFieldName', s:GroupAt(6, 3), 'a field called enum')
  bwipeout!
  call s:Open(['pkt TM {', '  temp: i16 ?= -160;', '};'])
  call assert_equal('fdslTypeName', s:GroupAt(2, 9), 'a type where a width goes')
  call assert_equal('fdslOperator', s:GroupAt(2, 13), 'the ?= of a default')
  bwipeout!
endfunction

" `@if' is an attribute name, and what follows it is a condition: `valof', the field it reads -- dotted
" through an instance -- and the comparison are what they are anywhere else.
function! s:TestConditionSyntax() abort
  call s:Open(['pkt TM {', '  [ @if: valof(hdr.flag) != 1; ] e: 8;', '};'])
  call assert_equal('fdslAttributeName', s:GroupAt(2, 6), 'if')
  call assert_equal('fdslKeyword', s:GroupAt(2, 10), 'valof')
  call assert_equal('fdslValofTarget', s:GroupAt(2, 20), 'the field, through its instance')
  call assert_equal('fdslOperator', s:GroupAt(2, 26), '!=')
  call assert_equal('fdslNumber', s:GroupAt(2, 29), 'the value')
  bwipeout!
  " Combined (#126): a mask, an ordering, `&&' and `||' are operators too.
  call s:Open(['pkt TM {', '  [ @if: valof(f) & 4 != 0 && valof(g) >= 2 || valof(g) < 1; ] e: 8;', '};'])
  call assert_equal('fdslOperator', s:GroupAt(2, 19), '&')
  call assert_equal('fdslOperator', s:GroupAt(2, 28), '&&')
  call assert_equal('fdslOperator', s:GroupAt(2, 40), '>=')
  call assert_equal('fdslOperator', s:GroupAt(2, 45), '||')
  call assert_equal('fdslOperator', s:GroupAt(2, 57), '<')
  call assert_equal('fdslNumber', s:GroupAt(2, 59), 'the value')
  bwipeout!
endfunction

" Reindenting every example changes nothing: the examples are what the formatter writes, so this says
" the indent file and the formatter agree about the layout.
function! s:TestIndent() abort
  for l:file in glob('examples/*.fdsl', 0, 1) + glob('examples/*/*.fdsl', 0, 1)
    execute 'edit!' fnameescape(l:file)
    let l:before = getline(1, '$')
    silent normal! gg=G
    call assert_equal(l:before, getline(1, '$'), 'reindenting ' . l:file)
    edit!
  endfor
  bwipeout!
endfunction

" The compiler plugin turns what the tool prints into a quickfix list.
function! s:TestQuickfix() abort
  call s:Open(s:sample)
  compiler fdsl
  call assert_match('frame-dsl', &makeprg, 'makeprg')
  cexpr! []
  cgetexpr ['defs.fdsl:4:3: error: expected '';'', found the name ''data''',
        \   'defs.fdsl:7:7: warning: block ''Spare'' is not used by anything [check 30]',
        \   'chatter that is not a diagnostic']
  let l:items = getqflist()
  call assert_equal(2, len(l:items), 'two diagnostics, and the chatter dropped')
  call assert_equal(4, l:items[0].lnum, 'line of the first')
  call assert_equal(3, l:items[0].col, 'column of the first')
  call assert_equal('e', l:items[0].type, 'the first is an error')
  call assert_equal('w', l:items[1].type, 'the second is a warning')
  call assert_match('not used by anything', l:items[1].text, 'the message survives')
  bwipeout!
endfunction

" Neovim's own client, from lsp/frame_dsl.lua -- attaches to a real 'frame-dsl --lsp' and gets a
" real diagnostic back. Plain Vim has no built-in client to test this way; editors/README.md
" documents its route through vim-lsp and ALE instead.
function! s:TestNeovimLsp() abort
  if !has('nvim')
    return
  endif
  " vim.lsp.start is Neovim 0.8's. Debian bookworm -- the image CI runs on -- ships 0.7.2, so there
  " this half skips rather than failing on a missing function; it says so, like a missing editor does.
  if !has('nvim-0.8')
    call add(s:skipped, 'the LSP test needs Neovim 0.8 or newer for vim.lsp.start, and this is '
          \ . matchstr(execute('version'), 'NVIM v\S\+'))
    return
  endif
  " A real path, unlike s:Open's scratch buffer: an unnamed buffer's URI is `file://' with nothing
  " after it, and the server checks it standalone rather than refusing it outright -- see
  " manual/editors.md -- but there is then no file for the diagnostic below to be about.
  execute 'edit!' fnameescape(tempname() . '.fdsl')
  call setline(1, ['pkt A {', '  valof: 8 := 1;', '};'])
  set filetype=fdsl
  let l:config = luaeval("dofile(vim.fn.getcwd() .. '/editors/vim/lsp/frame_dsl.lua')")
  let l:client = luaeval('vim.lsp.start(_A, {bufnr = 0})', l:config)
  call assert_notequal(v:null, l:client, "lsp/frame_dsl.lua's config starts a client")

  " 20s, not 5: the Emacs equivalent (editors/emacs/frame-dsl-mode-tests.el) gives the same
  " spawn-a-server-and-wait-for-a-diagnostic case 20s, and a loaded CI runner can take
  " noticeably longer than a local machine to start the subprocess and get a report back.
  let l:got = luaeval('vim.wait(20000, function() return #vim.diagnostic.get(0) > 0 end, 50)')
  call assert_true(l:got, 'the server reports the bad field within 20s')
  if l:got
    call assert_match('valof', luaeval('vim.diagnostic.get(0)[1].message'), 'the diagnostic names the problem')
  endif

  if l:client isnot v:null
    call luaeval('vim.lsp.stop_client(_A)', l:client)
  endif
  bwipeout!
endfunction

let s:skipped = []

call s:TestFiletype()
call s:TestSyntax()
call s:TestPastedSyntax()
call s:TestEnumSyntax()
call s:TestConditionSyntax()
call s:TestIndent()
call s:TestQuickfix()
call s:TestNeovimLsp()

" To stderr, not :echomsg: under -es a message is shown only with 'verbose' set, so a failure in CI
" would otherwise say nothing at all about which assertion it was.
for s:reason in s:skipped
  call writefile(['frame-dsl vim tests: skipped: ' . s:reason], '/dev/stderr')
endfor
if len(v:errors) > 0
  call writefile(v:errors, '/dev/stderr')
  cquit!
endif
echo 'frame-dsl vim tests: ok'
qall!
