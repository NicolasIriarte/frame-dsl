- **Repetition.** `points: Sample[2];` lays a named block out twice in a row, and
  `samples[48]: Sample[valof(count)];` as many times as a field above says, within the 48 bytes it
  reserves -- a frame claiming more is refused, decoding or encoding. An element's fields are
  `TM.samples[2].reading` to `framedsl_find()`, `-e` and `-d`; in C, `framedsl_element()` picks element
  *i* of a field found once, `framedsl_count()` says how many a frame holds, and `framedsl_member()`
  describes an element's fields. Each element keeps its block's fixed values, orders and calibrations.
  A repeated block must be one size and a whole number of bytes (check 32), and takes no overrides
  (check 33). `examples/sensor_samples.fdsl` is the worked example.
