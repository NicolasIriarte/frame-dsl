#!/bin/bash
#
# Runs a command with its output cut off after MAX_LINES lines, and fails it if that happens (#114).
#
# Usage: scripts/cap_output.sh MAX_LINES COMMAND [ARGUMENT...]
#
# Exits with the command's own status when it stays under the cap; with 1 when it does not. Its
# standard error is folded into standard output, which is where the cap is counted.
#
# A program in a loop can print far faster than any timeout can help with. ASan's startup collision on
# CI's kernel (#80) printed AddressSanitizer:DEADLYSIGNAL about 100,000 times a second until something
# killed it: one `test` job left 89 million lines, 5.3 GB, and two days of such runs grew the Forgejo
# database dump from 26 MB to 4.5 GB. A timeout bounds how long that goes on, not how much it writes.
#
# Past the cap nothing more is read, so the command's next write fails with SIGPIPE and, unless it
# ignores that, it stops there -- a runaway ends in a fraction of a second rather than when its timeout
# fires. Pick the cap well above anything the command prints when it works, so that crossing it can
# only mean something is wrong.
set -uo pipefail

if [ "$#" -lt 2 ] || ! [[ "$1" =~ ^[1-9][0-9]*$ ]]; then
	echo "usage: scripts/cap_output.sh MAX_LINES COMMAND [ARGUMENT...]" >&2
	exit 2
fi
max_lines="$1"
shift

# fflush() so that a line reaches the log when it is printed, not when a buffer fills: output that
# stops mid-run is exactly when somebody needs its last lines.
"$@" 2>&1 | awk -v max="$max_lines" 'NR > max { exit 1 } { print; fflush() }'
statuses=("${PIPESTATUS[@]}")

if [ "${statuses[1]}" -ne 0 ]; then
	echo "cap_output: more than $max_lines lines of output from \`$*\` -- cut here, and failed (#114)" >&2
	exit 1
fi
exit "${statuses[0]}"
