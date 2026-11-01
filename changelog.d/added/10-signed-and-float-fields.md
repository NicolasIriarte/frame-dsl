- **Signed and floating-point fields.** A type goes where a width goes: `temp: i16;` and `gain: f32;`, with
  `i1` to `i64`, `f32` and `f64`. The bytes on the wire are laid out as any field's are, and a plain number
  stays unsigned. `framedsl_get_i64()`, `framedsl_set_i64()`, `framedsl_get_f64()` and `framedsl_set_f64()`
  read and write a value, `framedsl_get_u64()` and `framedsl_set_u64()` stay raw bits on every scalar, a
  curve sees the value, `-d` prints a field by its type, and in Python `frame["temp"]` is a negative int,
  `frame["gain"]` a float, and `frame.raw[...]` the pattern. A value in a file is written as a number in
  the type's range, `offset: i8 := -1;`, and `valof()` may name a signed field. `FramedslField` gains `kind`,
  and grows, so a program has to be rebuilt.
