- **A warning for an attribute written twice in one place** (check 34). The same attribute twice in one
  `[ ... ]` group, or in the file's bare defaults -- `[ @byte_order: little_endian; @byte_order:
  big_endian; ]`, or two `@cal:` -- used to load without a word, the later silently winning. It still
  wins, so no file changes meaning, but `--check` and the language server now say so, and `--check
  --werror` refuses it. The quick fix removes the earlier of the two.
