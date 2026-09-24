# Performance

What encoding, decoding and calibration cost, measured by `make bench` (`bench/bench.c`). The
benchmark is a user of the library -- the public header and `build/libframedsl.a`, like
`examples/uart_device.c` -- so these are the costs a caller sees, per frame, including resolving the
packet by name and every check the API makes.

It is a measurement, not a test. Nothing fails on a slow result, and CI builds the benchmark without
running it: a timing on a shared runner is noise. Compare numbers from the same machine, before and
after a change. `make bench BENCH_SECONDS=1` runs each measurement for longer.

## First numbers, 2026-09-21

Intel Core i5-9400T at 1.80 GHz (frequency scaling on), Linux 6.8, gcc 13.3 at `-O2`. Each figure is
one operation on one frame; each measurement ran for at least half a second.

| Operation | Packet | Time |
| --- | --- | --- |
| encode | 8 bytes, whole-byte fields | 183 ns |
| decode | same | 183 ns |
| encode | 9 bytes, sub-byte fields and a little-endian one | 227 ns |
| decode | same | 214 ns |
| encode | 203 bytes, a `valof()` payload of 200 | 3166 ns |
| decode | same | 1803 ns |
| encode | 19 bytes, built from two blocks and a calibrated pair | 411 ns |
| decode | same | 368 ns |
| `get_eng` | a linear calibration | 14 ns |
| `get_eng` | a calibration run as bytecode | 23 ns |
| `set_eng` | a linear calibration, inverted automatically | 17 ns |
| `set_eng` | an explicit `inverse`, run as bytecode | 31 ns |

Two runs on this machine differed by up to half as much again on the smallest packets (a whole-byte
decode measured 121 ns in one and 183 ns in the next), so treat differences under that as noise.

## After copying whole bytes (#62), same machine

Fields now move up to a byte at a time -- a copy on a byte boundary, shifted bytes off one, `msb_first`
scalars in runs of up to eight bits -- instead of one bit at a time.

| Operation | Packet | Before | After |
| --- | --- | --- | --- |
| encode | 8 bytes, whole-byte fields | 183 ns | 67 ns |
| decode | same | 183 ns | 86 ns |
| encode | 9 bytes, sub-byte fields and a little-endian one | 227 ns | 110 ns |
| decode | same | 214 ns | 117 ns |
| encode | 203 bytes, a `valof()` payload of 200 | 3166 ns | 71 ns |
| decode | same | 1803 ns | 94 ns |
| encode | 19 bytes, built from two blocks and a calibrated pair | 411 ns | 137 ns |
| decode | same | 368 ns | 132 ns |

A payload now costs about what the packet around it does. `lsb_first` scalars still go a bit at a time:
the order is the reverse of the stream's, and it is rare.

## What the first numbers said

- **Payloads cost about 15 ns a byte to encode and 9 ns to decode, and dominate every frame that has
  one.** Every field is written and read one *bit* at a time, including a byte-aligned run of bytes
  that could be copied whole. That was the obvious first optimisation; it is #62, measured above.
- **Calibrations are cheap next to a frame.** A linear curve is a multiply and an add, and costs less
  than a tenth of decoding the smallest packet; the bytecode interpreter roughly doubles that. The
  generated C (`--emit-c`) was not measured here, because it is whatever the caller's compiler makes
  of one expression.
- **Composition costs nothing of its own.** A composed packet is the packet written out, and costs
  what its 19 bytes of fields cost.
- **Encode looks the packet up by name on every call.** On these small definitions that is a handful
  of string compares and is lost in the bit loop; it becomes worth a pre-resolved handle only once the
  copying is fast, or a definition has many packets.

## Repetition (#55), same machine, 2026-09-24

Added once repetition landed: `TM_SAMPLES`, a counted repetition of 32 three-byte `Sample` elements
(98 bytes a frame), each with a fixed 4-bit marker and a little-endian 16-bit reading. Three runs agreed
to within about 10%.

| Operation | Packet | Time |
| --- | --- | --- |
| encode | 98 bytes, 32 elements, one element's reading set per frame | 600--650 ns |
| decode | same | 89--100 ns |
| `framedsl_element` + `get_u64` | one element's reading | 60 ns |
| `framedsl_count` + 32 of the above | every element's reading | 2000 ns |

- **Decoding a repetition costs what any payload does.** Its elements are copied in as the bit string
  they are, and nothing looks inside until somebody asks.
- **Encoding one is slow, and most of it is the fixed values.** After copying the run, encode writes
  every element's fixed marker from the definition, a bit at a time through `frame_member_put()`. The
  same benchmark without a fixed value in `Sample` encodes in about 270 ns, so the stamping is over half
  the cost, about 10 ns per element here. A whole-byte fixed field could be written a byte at a time,
  as #62 did for ordinary fields.
- **Reading an element costs as much as decoding a small frame.** 60 ns a field: `framedsl_element()`
  and the accessor each validate the id and recompute how many elements the repetition has room for,
  and the member is read a bit at a time. A loop over a decoded repetition pays that per element, so
  reading all 32 costs twenty times the decode that brought them in. Byte-aligned members read as a
  load, and a capacity kept in the element program rather than recomputed, are the two obvious fixes.
