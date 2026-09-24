- **Hover, go to definition and completion in the language server.** Hovering a field shows its
  width, its offset in the packet and the bit and byte order in force after every attribute scope;
  hovering a block, a calibration, an instance, a namespace or an attribute says what it is. Go to
  definition follows `valof()` targets, blocks, calibrations and override names, into an imported file
  and through an `as` namespace. Completion offers attribute names and values, the scalars a `valof(`
  may name, block and calibration names, what an `ns.` brings, a block's fields inside its overrides,
  and the keywords. `manual/editors.md` says what each shows.
