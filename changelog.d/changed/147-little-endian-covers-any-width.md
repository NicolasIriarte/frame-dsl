- **`little_endian` covers a field of any width, and a sub-byte field that inherits it moves.** Fields now
  fill a byte from its low bit and run on into the next, as a packed C struct does on a little-endian
  processor and a CAN signal in Intel format, so `value: 12; flags: 4;` reads a word it could not before.
  Whole bytes on a byte boundary keep their bytes, and nothing big-endian changes. A sub-byte field under
  `little_endian`, or a whole-byte one off a boundary, used to ignore it, so its bits move with no error
  at load: list such fields lowest first, or say `big_endian` on them. Check 10 now refuses two orders
  sharing a byte, and no longer refuses `@byte_order` on a sub-byte field.
