- **A width or a count read from a fixed field reads its fixed value.** Encoding sized a field from what
  a fixed field's slot held -- zero, unless set -- rather than from the value it writes and a decoder
  reads, so `d[4]: valof(n) * 8;` after `n: 8 = 2;` encoded differently from how it decoded.
  `framedsl_count()` had the same mistake.
