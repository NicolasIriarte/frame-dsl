" Vim syntax file for Frame-DSL definition files.
"
" Regular expressions, like the Emacs mode: this is what a buffer looks like in the moment before
" `frame-dsl --lsp' answers with the compiler's own classification. It knows the shape of the language
" and not its meaning, so a field named like an attribute value is coloured as one.
"
" The three word lists below are the only place a word is written down, and
" test/unit/9_editor_support.c compares them against src/lexer.c and src/parser.c. Add a word here and
" nowhere else.
if exists("b:current_syntax")
  finish
endif

" Reserved words: none of these may be used as an identifier.
syntax keyword fdslKeyword pkt valof block
" Attribute names this version of the language knows. Not reserved: meaningful only after `@'.
syntax keyword fdslAttributeName bit_order byte_order cal contained
" Values an attribute may take. Not reserved either.
syntax keyword fdslAttributeValue msb_first lsb_first big_endian little_endian contained

" Words that are words only where they stand: a field may still be called `import' or `inverse'.
syntax match fdslContextual "^\s*\<import\>"
syntax match fdslContextual "^\s*\<cal\>"
syntax match fdslContextual "\<as\>\ze\s*\w*\s*;"
syntax match fdslContextual "\<inverse\>\ze\s*("

" `@name: value;' -- the name and the value are highlighted only here, which is what makes a field
" called `bit_order' legal and uncoloured. An unknown name is painted as an error, so a typo looks
" wrong before it is compiled.
syntax match fdslAttributeSigil "@" nextgroup=fdslAttributeName,fdslAttributeUnknown
syntax match fdslAttributeUnknown "\<\w\+\>" contained
" The value is whatever follows the colon: a known one is a keyword and wins, anything else is the
" name of a calibration, which is the only attribute whose value is a name.
syntax match fdslAttributeColon ":" contained nextgroup=fdslAttributeValue,fdslCalName skipwhite
syntax region fdslAttribute start="@" end=";" keepend
      \ contains=fdslAttributeSigil,fdslAttributeName,fdslAttributeUnknown,fdslAttributeColon,fdslComment

" The name a calibration is given, and the name a use refers to: `@cal: volts;' and `hdr: Header;'.
" Look-behind rather than \zs: a match may not start where a keyword has already matched, and every
" one of these follows a keyword.
syntax match fdslCalName "\a\w*\%(\.\w\+\)*" contained
syntax match fdslCalDecl "\%(^\s*cal\s\+\)\@<=\a\w*"
syntax match fdslPacketName "\%(\<\%(pkt\|block\)\>\s\+\)\@<=\a\w*"
syntax match fdslFieldName "^\s*\zs\w\+\ze\s*\%(\[[^]]*\]\)\?\s*:"
syntax match fdslTypeName "\%(:\s*\)\@<=\a\w*\%(\.\w\+\)*\ze\s*[;{]"
" A block pasted in without an instance name, `Header { ... };` or `Header;`, alone on its line. The
" `block {' of an attribute scope has the same shape, and stays a keyword: a keyword always wins.
syntax match fdslTypeName "^\s*\zs\a\w*\%(\.\w\+\)*\ze\s*[;{]"
syntax match fdslValofTarget "\%(\<valof\s*(\s*\)\@<=\w\+"
syntax match fdslNamespace "\%(\<as\s\+\)\@<=\a\w*"

syntax match fdslNumber "\<0[xX]\x\+\>"
syntax match fdslNumber "\<0[bB][01]\+\>"
syntax match fdslNumber "\<\d\+\%(\.\d*\)\?\%([eE][-+]\?\d\+\)\?\>"
syntax match fdslOperator "[-+*/=]"
syntax region fdslString start=+"+ end=+"+ oneline
syntax match fdslComment "#.*$" contains=@Spell

highlight default link fdslKeyword Keyword
highlight default link fdslContextual Keyword
highlight default link fdslAttributeSigil PreProc
highlight default link fdslAttributeName PreProc
highlight default link fdslAttributeUnknown Error
highlight default link fdslAttributeValue Constant
highlight default link fdslCalName Function
highlight default link fdslCalDecl Function
highlight default link fdslNamespace Identifier
highlight default link fdslPacketName Type
highlight default link fdslTypeName Type
highlight default link fdslFieldName Identifier
highlight default link fdslValofTarget Identifier
highlight default link fdslNumber Number
highlight default link fdslOperator Operator
highlight default link fdslString String
highlight default link fdslComment Comment

let b:current_syntax = "fdsl"
