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

## What the numbers say

- **Payloads cost about 15 ns a byte to encode and 9 ns to decode, and dominate every frame that has
  one.** Every field is written and read one *bit* at a time, including a byte-aligned run of bytes
  that could be copied whole. That is the obvious first optimisation, and it is #62.
- **Calibrations are cheap next to a frame.** A linear curve is a multiply and an add, and costs less
  than a tenth of decoding the smallest packet; the bytecode interpreter roughly doubles that. The
  generated C (`--emit-c`) was not measured here, because it is whatever the caller's compiler makes
  of one expression.
- **Composition costs nothing of its own.** A composed packet is the packet written out, and costs
  what its 19 bytes of fields cost.
- **Encode looks the packet up by name on every call.** On these small definitions that is a handful
  of string compares and is lost in the bit loop; it becomes worth a pre-resolved handle only once the
  copying is fast, or a definition has many packets.
