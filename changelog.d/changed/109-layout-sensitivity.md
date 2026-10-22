- **Decode time no longer depends on where the stack happens to sit.** A decode cleared a 2 KB array on
  the stack on every frame, and stores there could collide with the frame's loads; one packet's decode
  varied by up to 30% from one process to the next. It now varies by about 7%. `make layout` measures it.
