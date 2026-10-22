- **A bit string off a byte boundary is copied eight bytes at a time**, in both directions, instead of a
  byte at a time: a 4 KiB one encodes about 6x and decodes about 6x faster. The bytes on the wire do
  not change.
