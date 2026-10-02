- **`FramedslField` has one more member**, `condition`, at its end: what a conditional field is present
  under, or NULL. A program compiled against an earlier header and linked against this library passes a
  struct one pointer too short, so it must be rebuilt.
