- **`FramedslFieldId` has two more members**, `element` and `member`, for the fields of a repetition's
  elements. Code that treats it as opaque, as the header asks, is unaffected; code that builds one
  positionally, `{ 0, 0, 0 }`, should write `{ 0 }` instead. `FramedslField` gains `element_bits` and
  `member_count`.
