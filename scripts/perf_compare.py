#!/usr/bin/env python3
"""Compare two performance baseline results (#102): which points moved, and which are within noise.

    scripts/perf_compare.py BEFORE.csv AFTER.csv [--threshold PERCENT] [--all]

Both files are what `make baseline` writes. A point has moved when both of these hold:

  - its median changed by more than the threshold (5% unless --threshold says otherwise), and
  - the two runs' p10-p90 ranges do not overlap.

The first alone would call a noisy point's wobble a change; the second alone would call a real but
negligible shift on a very steady point one. Everything else is within noise. The report lists the
points that moved, slower first, then counts the rest; --all lists every point.

It always exits 0: this is a measurement to read, not a gate. It warns when the two files come from
different machines, compilers, flags, governors or modes, since then a difference may be that and not
the change being measured. Standard library only.
"""

import argparse
import csv
import sys

KEY = ("dimension", "variant", "value", "operation")
# Header lines that should match for two runs to be comparable. The commit and date are expected to
# differ, and are shown rather than checked.
COMPARABLE = ("cpu", "governor", "kernel", "compiler", "cflags", "mode", "sample_ms")


def read(path):
    header = {}
    lines = []
    with open(path, newline="") as handle:
        for line in handle:
            if line.startswith("#"):
                name, _, value = line[1:].partition(":")
                if value:
                    header[name.strip()] = value.strip()
            else:
                lines.append(line)
    rows = {}
    for row in csv.DictReader(lines):
        key = tuple(row[k] for k in KEY)
        rows[key] = {
            "median": float(row["median_ns"]),
            "p10": float(row["p10_ns"]),
            "p90": float(row["p90_ns"]),
        }
    return header, rows


def label(key):
    dimension, variant, value, operation = key
    return f"{dimension}/{variant}/{value} {operation}"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--threshold", type=float, default=5.0, help="percent change in the median (default 5)")
    parser.add_argument("--all", action="store_true", help="list every point, not only those that moved")
    options = parser.parse_args(argv)

    before_header, before = read(options.before)
    after_header, after = read(options.after)

    print(f"before: {options.before} (commit {before_header.get('commit', '?')}, {before_header.get('date', '?')})")
    print(f"after:  {options.after} (commit {after_header.get('commit', '?')}, {after_header.get('date', '?')})")
    for name in COMPARABLE:
        if before_header.get(name) != after_header.get(name):
            print(
                f"warning: {name} differs -- {before_header.get(name)!r} vs {after_header.get(name)!r}; "
                "a difference below may be that rather than the change",
                file=sys.stderr,
            )

    moved = []
    steady = []
    for key in sorted(set(before) & set(after), key=lambda k: (k[0], k[1], int(k[2]) if k[2].isdigit() else 0, k[3])):
        b, a = before[key], after[key]
        change = (a["median"] - b["median"]) / b["median"] * 100 if b["median"] > 0 else 0.0
        disjoint = a["p10"] > b["p90"] or b["p10"] > a["p90"]
        entry = (key, b, a, change)
        if abs(change) > options.threshold and disjoint:
            moved.append(entry)
        else:
            steady.append(entry)

    def show(entries, title):
        if not entries:
            return
        print(f"\n{title}")
        print(f"  {'point':<48} {'before ns':>10} {'after ns':>10} {'change':>8}   before p10-p90 / after p10-p90")
        for key, b, a, change in entries:
            print(
                f"  {label(key):<48} {b['median']:>10.1f} {a['median']:>10.1f} {change:>+7.1f}%"
                f"   {b['p10']:.1f}-{b['p90']:.1f} / {a['p10']:.1f}-{a['p90']:.1f}"
            )

    slower = sorted((e for e in moved if e[3] > 0), key=lambda e: -e[3])
    faster = sorted((e for e in moved if e[3] < 0), key=lambda e: e[3])
    show(slower, f"Slower ({len(slower)}):")
    show(faster, f"Faster ({len(faster)}):")
    if options.all:
        show(steady, f"Within noise ({len(steady)}):")

    only_before = sorted(set(before) - set(after))
    only_after = sorted(set(after) - set(before))
    print(
        f"\n{len(slower)} slower, {len(faster)} faster, {len(steady)} within noise "
        f"(median change over {options.threshold:g}% and p10-p90 ranges apart)."
    )
    if only_before or only_after:
        print(f"{len(only_before)} points only in before, {len(only_after)} only in after.")
        for key in only_before:
            print(f"  only before: {label(key)}")
        for key in only_after:
            print(f"  only after:  {label(key)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
