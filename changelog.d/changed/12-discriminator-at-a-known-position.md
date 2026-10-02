- **A packet's fixed value must sit where a decoder can find it.** Check 11 asked for a fixed value
  anywhere in a packet; it now asks for one at a known position, in a field every frame carries. A packet
  whose only fixed value came after a `valof()`-sized field used to load and then never decode; it is
  refused now, saying why.
