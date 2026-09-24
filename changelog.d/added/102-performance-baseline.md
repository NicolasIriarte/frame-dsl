- **A performance baseline.** `make baseline` sweeps encode, decode and field access over generated
  definitions -- packet size, fields per packet, repetitions, packets per definition and field shape --
  checking each definition round-trips before timing it, and writes the median, p10, p90, min and max
  of 300 samples per point, pooled from 20 processes, to a CSV that names the machine, compiler, flags
  and commit. `scripts/perf_compare.py before.csv after.csv` says which points moved. About 7.5
  minutes, run by hand; `make baseline-quick` takes seconds. See `docs/performance.md`.
