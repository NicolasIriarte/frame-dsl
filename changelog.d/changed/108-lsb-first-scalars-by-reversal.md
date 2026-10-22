- **`lsb_first` scalars are reversed and written like `msb_first` ones**, a byte at a time instead of a
  bit at a time: eight 16-bit `lsb_first` fields encode about 2.3x and decode about 1.5x faster. The
  bytes on the wire do not change.
