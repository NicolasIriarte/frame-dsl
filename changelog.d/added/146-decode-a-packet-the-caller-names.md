- **Decoding a packet the caller names.** `@decode: manual;` says the program decides which packet arrived,
  for a link that carries identity outside the bytes, as a CAN ID does. Such a packet needs no fixed field
  and may overlap any other, and `framedsl_decode_as()` reads it. `auto`, the default, is what every
  packet did before. `frame-dsl -d <hex> --packet NAME` does the same from the tool, and in Python
  `frame.decode(data, packet="NAME")`.
