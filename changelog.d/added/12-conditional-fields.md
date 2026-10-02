- **Conditional fields.** `[ @if: valof(sec_hdr) == 1; ]` in front of a field, a block's use or an
  anonymous `block { ... };` puts it on the wire only when an earlier field has that value -- or, with
  `!=`, has not. On a field typed by an enum the value is one of its names, `valof(kind) != IDLE`.
  Groups nest, and a nested part needs every condition around it. Presence is worked out from the
  frame's values rather than stored, so it means the same for a decoded frame and one being built:
  reading a field the frame does not carry is the new `FRAMEDSL_ERROR_ABSENT`, `framedsl_present()`
  asks without reading, a field can still be set while absent and waits for its condition, and a decode
  without a field clears it. `FramedslField.condition` shows what a field is present under, the
  description prints it, `-d` prints an absent field as `<absent>`, and Python has `frame.present()`
  and `AbsentError`. A conditional part must be whole bytes on its own (check 13), a `valof()` may read
  a conditional field only from under the same conditions (the new check 42), and a part of a repeated
  block may not be conditional (check 32). The language server navigates, describes and completes a
  condition. `examples/optional_fields.fdsl` is the worked example.
