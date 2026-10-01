- **`FramedslField` has one more member**, `enumeration`, at its end: the enum a field is typed by, or
  NULL. A program compiled against an earlier header and linked against this library passes a struct
  one pointer too short, so it must be rebuilt.
