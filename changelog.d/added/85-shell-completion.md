- **Bash and zsh completion.** `frame-dsl --<TAB>` offers the flags `--help` lists, a definition
  argument completes to `.fdsl` files, `-e <TAB>` to the packets of the file on the line, and
  `-e TC_WRITE <TAB>` to that packet's fields as `size=`, leaving out the ones already given.
  `make install` puts `completions/frame-dsl.bash` and `completions/_frame-dsl` where Bash and zsh
  look for them.
