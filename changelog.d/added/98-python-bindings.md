- **Python bindings.** `python/framedsl` loads a definition and encodes and decodes frames through the
  C library, over `ctypes`: `framedsl.load(path)`, `frame["PKT.field"]` (an int for a scalar, bytes
  for anything wider), `frame.eng[...]` for engineering values, `frame.count()` and `frame.elements()`
  for repetitions, `frame.encode(packet)` and `frame.decode(data)`. A definition that does not load
  raises `LoadError` carrying every diagnostic; each accessor error is an exception of its own. `make
  python` then `pip install ./python` installs it, with no network.
- **A shared library.** `make lib` builds `build/libframedsl.so` beside the static archive, versioned
  (`libframedsl.so.0`) and exporting only the `framedsl_*` API; `make install` installs it and
  `framedsl.h` into `$(PREFIX)/lib` and `$(PREFIX)/include`.
