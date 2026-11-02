- **A fixed value is written `:=`, and every definition file has to change.** `id: 8 = 0x40;` is now
  `id: 8 := 0x40;`, on a field and in an override, and a bare `=` there is an error with a quick fix.
  `=` stays for naming something: an enum's value and a calibration. Nothing else about a fixed value
  changes. `--format` also starts every value in a run in one column.
