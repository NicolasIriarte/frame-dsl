#!/bin/bash
#
# Checks scripts/cap_output.sh passes a well-behaved command through untouched, and stops a runaway one
# quickly, in a few lines, as a failure (#114).
#
# Usage: scripts/test_cap_output.sh      (`make test` runs it)
#
# The runaway is `yes` printing AddressSanitizer:DEADLYSIGNAL, which is what the real one printed (#80):
# ASan's own loop cannot be brought about on purpose on a machine whose runtime avoids it.
set -uo pipefail

cap="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/cap_output.sh"
failures=0

pass() { printf "  ok   %s\n" "$1"; }
fail() {
	printf "  FAIL %s\n" "$1"
	failures=$((failures + 1))
}

echo "scripts/cap_output.sh"

output="$("$cap" 3 printf 'one\ntwo\nthree\n')"
status=$?
if [ "$status" -eq 0 ] && [ "$output" = $'one\ntwo\nthree' ]; then
	pass "output at the cap passes through unchanged"
else
	fail "output at the cap passes through unchanged (status $status)"
fi

"$cap" 10 sh -c 'echo failing; exit 7' >/dev/null
status=$?
if [ "$status" -eq 7 ]; then pass "the command's own status comes back"; else fail "the command's own status comes back (got $status)"; fi

output="$("$cap" 10 sh -c 'echo to stderr >&2')"
if [ "$output" = "to stderr" ]; then pass "standard error is kept, and counted"; else fail "standard error is kept, and counted"; fi

output="$("$cap" 3 printf 'one\ntwo\nthree\nfour\n' 2>/dev/null)"
status=$?
if [ "$status" -eq 1 ] && [ "$output" = $'one\ntwo\nthree' ]; then
	pass "one line over the cap is cut, and fails"
else
	fail "one line over the cap is cut, and fails (status $status)"
fi

# The case that matters: a command that would print forever. Under `timeout` as the Makefile runs it, and
# with a far longer one than a pass needs, so a cap that only cut the output -- rather than stopping
# the command -- fails here instead of passing slowly.
start=$SECONDS
lines="$("$cap" 1000 timeout 60 yes AddressSanitizer:DEADLYSIGNAL 2>&1 | wc -l)"
status=${PIPESTATUS[0]}
elapsed=$((SECONDS - start))
if [ "$status" -eq 1 ] && [ "$lines" -le 1001 ] && [ "$elapsed" -lt 10 ]; then
	pass "a runaway is stopped at the cap, in seconds, as a failure"
else
	fail "a runaway is stopped at the cap, in seconds, as a failure (status $status, $lines lines, ${elapsed}s)"
fi

"$cap" 0 true >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then pass "a cap of zero is refused"; else fail "a cap of zero is refused (got $status)"; fi

"$cap" 10 >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then pass "no command is refused"; else fail "no command is refused (got $status)"; fi

if [[ "$failures" -ne 0 ]]; then
	printf "[FAIL] - %d output cap check(s) failed\n" "$failures" >&2
	exit 1
fi
echo "The output cap passes what it should, and stops what it should."
