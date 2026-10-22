- **Encoding and decoding cost less per field.** The fields at the start of a packet whose place and
  width the definition fixes -- every field, in most packets -- are worked out when it loads and moved
  without any per-field checks, and a decode of a packet laid out entirely that way no longer puts the
  previous frame's values aside first. A packet of 255 16-bit fields encodes and decodes in less than
  half the time it did. The bytes on the wire do not change.
