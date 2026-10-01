- **Enumerations.** `enum TxLevel: 3 { OFF; LOW; MID; HIGH; MAX = 7; };` names the values of a field
  typed by it, `level: TxLevel;`, which is the enum's width. A name without a value is one more than the
  name before it, the first is 0, and the values must increase. A fixed value or override on such a
  field is one of its names, written bare -- `type: PktType = HK;`, `Header { type = HK; }` -- so a
  discriminator reads as what it means. The field's bit and byte order are its own, from where it is
  used. `-d` prints each value's name, and marks one the enum does not list `<unlisted>` rather than
  refusing the frame. In C, `framedsl_get_name()` and `framedsl_set_name()` read and write a field by
  name -- an unlisted value, or an unknown name, is the new `FRAMEDSL_ERROR_UNLISTED` -- while
  `framedsl_get_u64()` and `framedsl_set_u64()` stay raw; `framedsl_enum_count()` and its siblings
  list every enum and its names, and `--emit-c` writes each as constants and a value-to-name function.
  Python has `frame.names[...]` and `definition.enums`. Enums import like blocks, flat or under `as`,
  and the language server highlights, describes and completes them. Checks 35 to 41 cover an enum's
  width, its names and values, and what a field typed by one may carry. `examples/radio_status.fdsl` is
  the worked example.
