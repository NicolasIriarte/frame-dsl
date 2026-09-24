- **Quick fixes in the language server.** A diagnostic with one obvious repair carries it, and the
  editor offers it as a quick fix: "did you mean" for a misspelt attribute name or value, `valof()`
  target, block or calibration (within two edits, and only when one name is closest); inserting a
  missing `;`; removing an unused block, calibration or import, or a redundant attribute and its
  group when that leaves it empty. `--check` prints the same suggestion as a `note:` line under the
  diagnostic, and `FramedslDiagnostic` gains `fix_title`, `fix_text` and the range it replaces.
  `manual/editors.md` lists each fix.
