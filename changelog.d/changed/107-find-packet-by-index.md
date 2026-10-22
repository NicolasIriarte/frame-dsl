- **Finding the packet no longer costs more the further down the file it is.** An encode looks the name
  up in a hash built when the definition loads, and a decode looks the frame up by the fixed bits every
  packet shares -- usually an id or a type -- then matches only the packets that agree, rather than
  trying each in turn: with 512 packets the last one now encodes and decodes in about what the first
  does, 40 times faster than before. Which packet a frame is does not change.
