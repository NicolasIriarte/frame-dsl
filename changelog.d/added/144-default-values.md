- **Default values.** `spare: 8 ?= 0;` gives a field what it holds until something replaces it. It is
  written into the slot when a frame is created and reset, a program or a decode replaces it, and it
  takes no part in identifying a packet, so a frame carrying another value still decodes. A use
  replaces one with the same words, `{ state ?= FAULT; }`, and each element of a repetition takes its
  own. `FramedslField` reports `has_default` and `default_value`, and grows, so a program has to be
  rebuilt.
