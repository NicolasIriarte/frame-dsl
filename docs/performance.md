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
  8 minutes, and the one to use to decide whether a change to the runtime is worth making. See
  [The baseline](#the-baseline) next.
- **`make layout`** (#109) times one decode with the stack and the frame's bytes moved across a page:
  seconds, for whether a change made decoding sensitive to where memory sits.

## The baseline

`bench/baseline.c` generates definitions that vary one thing at a time, with everything else held at a
small default, and times the public API on each. The last two dimensions are the exception: no real
definition looks like one point of the others, so `mix` combines features the way real definitions do,
and `grid` crosses a field's shape with its alignment and count -- a change that speeds one shape up at
another's expense shows up there.

| Dimension | What varies | Operations |
| --- | --- | --- |
| `size` | 16 B to 4 KiB on the wire: one fixed-width bit string (`fixed`), the same off a byte boundary (`fixed_unaligned`), a `valof()` payload (`valof`) | encode, decode |
| `fields` | 1 to 255 16-bit fields after the id | encode, decode, `set_u64`, `get_u64` |
| `elements` | 1 to 1024 elements of a 3-byte block, a constant count (`const`) or a counted one (`counted`) | encode, decode, `element_get`, `elements_all` |
| `element_bytes` | 32 counted elements of 1 to 32 one-byte members | the same |
| `element_shape` | 32 counted 3-byte elements: plain, with a fixed value, `little_endian`, `lsb_first`, sub-byte | the same |
| `packets` | 1 to 512 packets in the definition; the first and the last declared | encode, decode |
| `shape` | eight fields of one shape: 3 to 64 bits, sub-byte, `lsb_first`, `little_endian`, calibrated (linear and bytecode) | encode, decode, `set_u64`, `get_u64`, `set_eng`, `get_eng` |
| `mix` (#130) | definitions written the way real ones are, each combining features: a CCSDS family of 32 apids and a telecommand family of 16 told apart three bytes in (first and last of each); housekeeping with an enum, `little_endian` calibrated and 12-bit calibrated fields; `lsb_first` fields and a bit string off a byte boundary; a counted repetition of mixed members; optional parts all present and all absent; a `valof()` payload off a byte boundary; 63 fields of nine shapes | encode, decode, and `get_u64`, `get_eng`, `set_eng`, `element_get`, `elements_all` where the definition has them |
| `grid` (#130) | eight or 64 fields of one shape (`u16`, `u12`, `lsb_first` `u16`, `little_endian` `u32`, calibrated `u16`), starting on a byte boundary (`_at0`) or three bits past one (`_at3`); `little_endian` only at 0, since check 10 refuses it off a boundary | encode, decode |

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

### Before the performance round, 2026-10-21

[`performance/2026-10-21-944c7b9-before.csv`](performance/2026-10-21-944c7b9-before.csv): `944c7b9`,
whose runtime is `main`'s at v0.1.0, the first run with `mix` and `grid` (#130). Same i5-9400T, but
Linux 7.0 and gcc 15.2 where the first baseline had 6.8 and 13.3, and the runtime has gained conditional
fields (#12) since: every accessor now asks whether its field is present. 191 of the 418 points the two
files share are slower than in the first baseline and 6 faster -- every `get_u64` by 32%, 7.6 to 10.1
ns -- and nothing tells those three changes apart. So this file, not the first, is what the performance round
(#105, #108, #101, #106, #107, #109) is measured against.

What the combinations cost, medians in ns:

| Point | encode | decode | What it says |
| --- | --- | --- | --- |
| `ccsds_family`, 29 B, first / last of 32 apids | 126 / 255 | 148 / 217 | the last apid pays ~4 ns per packet ahead of it, both ways |
| `telecommands`, 8 B, first / last of 16, told apart 3 bytes in | 60 / 126 | 92 / 140 | the same, on a family whose first byte is shared |
| `housekeeping`, 29 B, enum, `little_endian`, calibrated, 12-bit | 194 | 215 | 18 fields at ~11 ns each |
| `unaligned_lsb_first`, 74 B | 271 | 237 | four `lsb_first` fields and a 64-byte unaligned bit string |
| `repetition`, 198 B, 32 mixed elements | 634 | 111 | `element_get` 74 ns, `elements_all` 2275 ns |
| `conditional_all` / `conditional_none`, 37 / 2 B | 179 / 81 | 173 / 123 | |
| `valof_unaligned`, 203 B | 338 | 371 | a 200-byte payload 5 bits into a byte |
| `wide`, 148 B, 63 fields of nine shapes | 1010 | 910 | ~15 ns a field |

### Bit strings off a byte boundary a word at a time (#105), 2026-10-21

[`performance/2026-10-21-57c60cb-105.csv`](performance/2026-10-21-57c60cb-105.csv). Off a byte
boundary, a bit string now moves eight bytes a step, two steps a loop, instead of one byte: the source
as a big-endian word shifted by the offset. Encode carries the bits spilling into the next byte in a
register rather than OR'ing them into memory -- a first version did that, and every step's one-byte
store was read back by the next step's eight-byte load, which cannot be forwarded and stalled encode at
half the speed of decode. Against the file before the round: 0 points slower, 47 faster.

| Point | Before | After | |
| --- | --- | --- | --- |
| `size/fixed_unaligned/4096` encode | 6026 | 1031 | 5.8x (6.0x on the first baseline's 6154) |
| `size/fixed_unaligned/4096` decode | 5181 | 854 | 6.1x (5.3x on the first baseline's 4559) |
| `size/fixed_unaligned/256` encode / decode | 382 / 400 | 113 / 133 | |
| `mix/valof_unaligned/203` encode / decode | 338 / 371 | 122 / 151 | |
| `mix/unaligned_lsb_first/74` encode / decode | 271 / 237 | 202 / 168 | the bit string's share; the `lsb_first` fields are #108 |

An unaligned 4 KiB bit string still costs about 6x the aligned one (161 / 151 ns here), which is a
`memcpy`; the shift is a handful of instructions a word, two of them variable shifts.

### `lsb_first` scalars by reversal (#108), 2026-10-21

[`performance/2026-10-21-1fbee8d-108.csv`](performance/2026-10-21-1fbee8d-108.csv). An `lsb_first`
value is its bit-reversal laid down `msb_first`, so it is reversed -- three swaps of ever smaller pieces
and a byte swap, about ten instructions -- and goes out in the same runs of up to eight bits, at any
offset. Decode reads the runs and reverses the result.

| Point | Before | After | vs `msb_first` |
| --- | --- | --- | --- |
| `shape/u16_lsb_first` encode / decode | 319 / 230 | 136 / 152 | 1.21x / 1.13x (`u16`: 112 / 134) |
| `shape/u12_lsb_first` encode / decode | 256 / 188 | 136 / 153 | |
| `grid/u16_lsb_first_at0/64` encode / decode | 2346 / 1392 | 925 / 853 | |
| `mix/unaligned_lsb_first/74` encode / decode | 202 / 168 | 126 / 150 | (after #105) |

Against the file before the round, 51 points are faster and 2 slower: `size/fixed/384` and `/512`
encode, by 7%, a `memcpy` this change does not touch. Against #105's run, `fields/u16` decode moved by
2-8% (32 fields the most), which a second, separate run of both binaries reproduced. Neither is in code
the change made slower; both look like the layout sensitivity #109 is about, and #106 rewrites that
path anyway.

### Repetition elements (#101), 2026-10-22

[`performance/2026-10-22-4b1b66a-101.csv`](performance/2026-10-22-4b1b66a-101.csv). A member is read and
written through the routines ordinary fields use, now in `src/bits.h`, instead of a bit at a time; those
gather a field's bytes into one word and shift once, and read a byte-aligned 8-, 16-, 32- or 64-bit
field straight off its bytes. An element's fixed values are worked out at load as a few masked bytes,
stamped into every element of a repetition on a byte boundary. The capacity is computed once, and a
member id is checked in one walk. Against the file before the round: 0 points slower, 358 faster.

| Point | Before | After | |
| --- | --- | --- | --- |
| `element_shape/bench_sample/32` encode | 565 | 116 | 4.9x |
| `mix/repetition/198` encode | 634 | 131 | 4.8x |
| `elements/counted/1024` encode | 15900 | 1914 | 8.3x |
| `element_bytes/u8_members/32` encode | 1066 | 71 | 15x: no fixed values, nothing to stamp |
| `element_get` (any shape) | 73.5 | 21.3 | 3.5x |
| `elements_all`, 1024 elements | 71183 | 18424 | 3.9x |
| `get_u64`, an ordinary field | 10.1 | 8.5 | the member path is out of line, so this one's entry is lean |

#101 asked for an element read within 2x of an ordinary `get_u64`. It is 2.5x the ordinary read as it is
now, and 2.1x the one before this change. What is left is the walk itself: `framedsl_element()` and
the read each go from the definition to the packet, the field, the element and the member, a chain of
dependent loads; an ordinary read does it once.

### The per-field cost (#106), 2026-10-22

[`performance/2026-10-22-8cda2dc-106.csv`](performance/2026-10-22-8cda2dc-106.csv). Profiled first, and
written up on #106: gprof, since `perf` is not allowed on this machine. Per field, decode asked whether
the field was present, worked out its width and checked it against the buffer; encode first copied every
value into an array only `valof()` widths and conditions read; and decode put the whole packet aside
before starting, in case it failed part-way. Each packet's **known prefix** -- the leading fields at a
constant offset, of a constant width, without a condition, which is every field of most packets -- is now
worked out at load, with each field's offset and how it moves. Decode checks the frame's length against
the prefix once and reads it with no per-field checks, and a packet that is all prefix is not put aside:
past that one check it cannot fail. Encode writes the prefix straight from the arena, and clears only what
its packet can take. Against the file before the round: 0 points slower, 469 faster.

| Point | Before | After | |
| --- | --- | --- | --- |
| `fields/u16/255` encode / decode | 2848 / 2721 | 1205 / 1040 | 2.4x / 2.6x (2.2x / 2.4x on the first baseline's 2656 / 2473) |
| `fields/u16/1` encode / decode | 37 / 67 | 27 / 19 | the fixed cost of a frame |
| `size/fixed/16` encode / decode | 45 / 73 | 32 / 24 | |
| `mix/housekeeping/29` encode / decode | 194 / 215 | 96 / 90 | |
| `mix/wide/148` encode / decode | 1010 / 910 | 412 / 423 | |

One measurement worth keeping: a first version gathered the values after the prefix rather than before,
for the same work, and a short conditional packet's encode (`mix/conditional_none`) came out 31% slower
than before the round; gathering first puts it back where it was, 81 to 79 ns. Against #101's run, `elements/const/32` encode
went from 91 to 110 ns, still a fifth of where it started.

### Finding the packet (#107), 2026-10-22

[`performance/2026-10-22-c7af840-107.csv`](performance/2026-10-22-c7af840-107.csv). A definition carries
an index built at load (`src/dispatch.c`). Names are hashed, so an encode costs the same whichever packet
it names. For decode, the bits every identifiable packet fixes, over the bytes all their discriminators
cover, are a key -- usually an id, a type or an apid -- and packets are grouped by their value under it:
a frame's key names the few packets to match in full. Definitions of up to four packets, and packets that
share no fixed bit, still try each in turn, which for them is as quick. Against the file before the
round: 0 points slower, 477 faster.

| Point | Before | After | |
| --- | --- | --- | --- |
| `packets/last/512` encode | 2281 | 58 | 1.6x the first packet's 35 (target: within 2x) |
| `packets/last/512` decode | 1121 | 33 | the same as the first packet's 33 |
| `mix/ccsds_family_last/29` decode | 217 | 90 | |
| `mix/telecommands_last/8` decode, told apart three bytes in | 140 | 33 | |

The index has a cost of its own, about 8 ns a decode, which a family's *first* packet now pays where
trying in turn found it at once: `packets/first/*` decode went from 24 to 33 ns against #106's run. Two
lookups inlined into the encoder and the decoder made every frame of a one-packet definition 3-4 ns
slower, for code those never run; the keyed search is out of line, and their decode is back to #106's.
Against #106's run some one-packet decodes still moved, either way -- `shape/u64_little_endian` 18%
slower, `shape/u8` 15% faster -- in code this change did not touch, which a second, separate run of both
binaries reproduced. That is #109's question.

### Why decode time moved with memory layout (#109), 2026-10-22

The first baseline saw one decode, `size/fixed/256`, measure anything from 67 to 86 ns from one process
to the next, and not move with address randomisation off: the reason the baseline pools 20 processes.
`perf` is not allowed on this machine, so the measurement is of decode time against where things sit,
one hypothesis at a time, written up on #109. On main's runtime before the round:

- **Across processes** the heap is laid out the same every time: the frame and the caller's buffer land
  at the same page offsets in every run. Only the stack moves -- and 80 runs spread from 66 to 103 ns.
- **The caller's buffer against the frame's own buffers**, the obvious suspect, is not it: moving the
  buffer across a page in one process moves the decode by 5%.
- **The stack is**: moving it across a page moves the decode by 13% in one process, with a slow band and
  a fast one. Every decode cleared a 2 KB array of field values on the stack and wrote an entry per field,
  while reading the frame and writing the arena on the heap -- a store and a load to addresses the same
  modulo 4096 cannot be told apart early, and the load waits.

#106 stopped clearing the array, and a decode of a fixed-layout packet no longer puts its old values
aside. After it, the same decode spreads 23.4-25.2 ns over 20 processes, and moving the stack or the
buffer across a page moves it 4-7%. The baselines' spread for decode, p10-p90 as a share of the median:

| | Before the round | After #107 |
| --- | --- | --- |
| `size/fixed` decode, mean / worst | 20.7% / 31.6% | 7.2% / 8.8% |
| `size/valof` decode, mean / worst | 13.1% / 20.7% | 5.8% / 7.2% |
| every decode point, mean / worst | 14.4% / 31.8% | 6.5% / 12.0% |
| `size/fixed` encode, which never moved | 7.1% / 15.7% | 6.9% / 15.9% |

**`make layout`** (`bench/layout.c`) repeats the in-process half: one decode with the stack, then the
frame's bytes, moved 64 bytes at a time across a page. A change that brings the sensitivity back shows up
there as a spread well past 7%. The baseline still pools 20 processes; the cost is the same, and code
alignment still moves single points between *builds* -- by up to 18% this round, in code a change did not
touch -- which no number of processes averages out.

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
