- **Writing a fixed field is refused rather than silently ignored.** `framedsl_set_u64()`,
  `framedsl_set_name()` and `framedsl_set_eng()` used to return `FRAMEDSL_OK` for a field with a fixed
  value, and the frame went out with the definition's value anyway. They now return the new
  `FRAMEDSL_ERROR_FIXED`, for every write, the value the field already has included, and Python raises
  `FixedError`. A fixed field also reads its fixed value on a new or reset frame, where it read zero.
  The bytes of an encoded frame do not change. A program that writes a fixed field now gets an error,
  so a loop copying fields should check `FramedslField.has_fixed`.
