# Performance

What encoding, decoding and calibration cost. Both benchmarks here are users of the library -- the
public header and `build/libframedsl.a`, like `examples/uart_device.c` -- so these are the costs a
caller sees, per frame, including resolving the packet by name and every check the API makes.

They are measurements, not tests. Nothing fails on a slow result, and CI builds the benchmarks without
running them: a timing on a shared runner is noise. Compare numbers from the same machine, before and
after a change.

There are two tools, for two questions:

- **`make bench`** times a dozen fixed operations, once each, on one hand-written definition: seconds,
  enough to see a gross regression. Its history is the tables further down.
- **`make baseline`** (#102) sweeps generated definitions and reports a distribution per point: about
  7.5 minutes, and the one to use to decide whether a change to the runtime is worth making. See
  [The baseline](#the-baseline) next.

## The baseline

`bench/baseline.c` generates definitions that vary one thing at a time, with everything else held at a
small default, and times the public API on each:

| Dimension | What varies | Operations |
| --- | --- | --- |
| `size` | 16 B to 4 KiB on the wire: one fixed-width bit string (`fixed`), the same off a byte boundary (`fixed_unaligned`), a `valof()` payload (`valof`) | encode, decode |
| `fields` | 1 to 255 16-bit fields after the id | encode, decode, `set_u64`, `get_u64` |
| `elements` | 1 to 1024 elements of a 3-byte block, a constant count (`const`) or a counted one (`counted`) | encode, decode, `element_get`, `elements_all` |
| `element_bytes` | 32 counted elements of 1 to 32 one-byte members | the same |
| `element_shape` | 32 counted 3-byte elements: plain, with a fixed value, `little_endian`, `lsb_first`, sub-byte | the same |
| `packets` | 1 to 512 packets in the definition; the first and the last declared | encode, decode |
| `shape` | eight fields of one shape: 3 to 64 bits, sub-byte, `lsb_first`, `little_endian`, calibrated (linear and bytecode) | encode, decode, `set_u64`, `get_u64`, `set_eng`, `get_eng` |

`element_get` is `framedsl_element()` and `framedsl_get_u64()` on one element; `elements_all` is
`framedsl_count()` and that loop over every element. `build/baseline --dump <dimension>` prints the
generated definitions.

**Nothing is timed until it is checked.** Every generated definition is loaded, and one frame of each
packet timed is encoded, compared against the length the generator meant, decoded into a second
frame, compared field by field (and element by element), and re-encoded to the same bytes. A
generator that broke stops the run rather than producing a fast, meaningless number, and so does any
call that fails while it is being timed.

**How a point is measured.** Each sample is a batch of iterations sized to take about 2 ms, so the
clock's cost disappears, preceded by a tenth of a batch untimed. Samples are taken in rounds -- one of
every point, in a shuffled order, then the next round -- so a drift in the machine spreads over every
point. The run pins itself to one CPU. And the samples come from **20 separate processes of 15
each**, pooled into 300 per point: the median, p10, p90, min and max in the CSV are over all 300.

The separate processes are the part that matters most. With every sample taken in one process, two
runs of the *same binary* disagreed on 94 of 418 points, every one of them a decode, by 5-8%. Decoding
one 256-byte packet measured anything from 67 to 86 ns depending on the process, and was steady with
address randomisation off (`setarch -R`). Where exec() places the stack, heap and libraries moves that
path by up to 30%. Turning randomisation off would give a steady number for one arbitrary layout, and
any code change would shift the layout and look like a change in speed. Pooling many processes puts
that variation in the spread instead, where it belongs. Two full runs of the same binary then agree on
all 418 points, and so do two quick runs on all 108. A library built at `-O1` instead of `-O2` is
flagged on 40 of the 108 quick points (29 slower by up to 23%, 11 faster).

### Running it

```sh
make baseline          # ~7.5 min: build/performance/<date>-<commit>.csv, and a table on stdout
make baseline-quick    # ~10 s: 3 values per dimension, 3 processes of 7 samples
make baseline BASELINE_ARGS="--only packets"   # one dimension; build/baseline --help for the rest
scripts/perf_compare.py before.csv after.csv   # which points moved
```

Run it with the machine otherwise idle, and preferably with the `performance` governor; it records the
governor and warns when it is not `performance`. It is not run in CI, which only builds it: timing on a
shared runner is noise, and it takes minutes.

`scripts/perf_compare.py` reports a point as **moved** only when its median changed by more than 5%
(`--threshold`) *and* the two runs' p10-p90 ranges do not overlap; everything else is within noise.
It warns when the files differ in machine, governor, compiler, flags or mode. It always exits 0.

### When to run it

Before and after any change to the language, the layout, the bytecode, or the encode/decode walk --
anything under `src/` that a frame passes through -- on the same machine, in the same state. Keep the
before file. A change proposed as a speed-up is backed by a comparison, not a single run. Record a new
baseline here when a change that moves numbers lands: copy its CSV into `docs/performance/` and update
the summary below.

### First baseline, 2026-09-24

[`performance/2026-09-24-ef14713.csv`](performance/2026-09-24-ef14713.csv): commit `ef14713` (main's
runtime at `4d627c6`), same i5-9400T as below, gcc 13.3 at `-O2`, `powersave` governor, 418 points in
448 s. Medians, in ns:

| Point | Median | What it says |
| --- | --- | --- |
| encode / decode, 16 B, whole bytes | 40 / 71 | the fixed cost of a frame |
| encode / decode, 4 KiB fixed-width bit string | 157 / 147 | a byte-aligned payload is a copy: ~0.03 ns a byte |
| encode / decode, 16 B vs 4 KiB `valof()` payload | 64 / 95 vs 151 / 170 | ~25 ns over fixed-width at 16 B, level by 4 KiB |
| encode / decode, 4 KiB bit string **off a byte boundary** | 6154 / 4559 | **~1.5 and ~1.1 ns a byte: 30-40x the aligned copy** |
| encode / decode, 1 field vs 255 fields | 35 / 64 vs 2656 / 2473 | **~10 ns per field**, in both directions |
| `set_u64` / `get_u64`, any field | 9 / 8 | flat: resolved ids cost nothing more with more fields |
| encode, 16-bit fields: `msb_first` vs `lsb_first` | 104 vs 345 | `lsb_first` still goes a bit at a time |
| encode / decode, 64-bit fields: big- vs little-endian | 239 / 243 vs 294 / 333 | the swap costs ~7 and ~11 ns a field |
| `get_eng` / `set_eng`, linear vs bytecode curve | 16 / 23 vs 29 / 41 | |
| encode / decode, first vs last of 512 packets | 50 / 78 vs 2098 / 1525 | **~4 ns and ~3 ns per packet ahead of it**: encode looks the name up, decode tries each in turn |
| encode / decode, 1024 elements | 16651 / 118 | encode **~16 ns per element**; decode is a copy |
| encode, 32 elements plain vs with a fixed 8-bit member | 186 vs 919 | stamping fixed values is ~23 ns an element |
| `element_get` | 60 | whatever the element's size or shape |
| `elements_all`, 1024 elements | 59875 | ~58 ns an element, 500x the decode |

## `make bench`: first numbers, 2026-09-21

`make bench BENCH_SECONDS=1` runs each measurement for longer.

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
