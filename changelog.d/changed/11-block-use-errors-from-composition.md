- **A block use's value, reservation or attribute group is refused by composition**, not the parser,
  since until every declaration is read the name could be an enum's, whose fields take all three. The
  messages are the same; a file with one of these mistakes and a syntax error elsewhere now reports the
  syntax error alone first.
