---
name: "Bug"
about: "Something the library or the CLI does, that it should not."
labels:
  - state/created
  - type/bug
---

# Description

What happens, and what should happen instead.

The `.fdsl` definition and the bytes involved, trimmed to the smallest case that still shows it --
`fuzz/regressions/` is where that trimmed case belongs once the fix lands, if a fuzz target covers
this code path.

Compiler and OS, since this is C: `cc --version` and `uname -a`. A bug that only reproduces under one
compiler or one set of sanitizer flags is a different bug from one that reproduces everywhere.

Whether it reproduces under `make fuzz` (see `fuzz/`) or only by hand -- and if a fuzz target already
covers this input, why it did not catch it.

# Test

- [ ] The steps above no longer reproduce it.
- [ ] A test that fails before the fix and passes after it.

# Tasks

- [ ] An action to be taken. Non-testable follow-ups go here rather than in Test.
