- **Repetitions encode and read much faster.** An element's members are read and written through the
  same routines as ordinary fields instead of a bit at a time, and its fixed values are stamped as a
  few masked bytes worked out when the definition loads: 32 three-byte elements with a fixed marker
  encode about 5x faster, and reading one element's field (`framedsl_element()` and
  `framedsl_get_u64()`) costs about a quarter of what it did. The bytes on the wire do not change.
