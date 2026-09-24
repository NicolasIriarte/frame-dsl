#!/bin/bash
#
# Checks scripts/check_conflict_markers.sh finds what it is for, and nothing else (#90).
#
# Usage: scripts/test_conflict_markers.sh      (`make test` runs it)
#
# A guard that never fires looks exactly like a clean tree, so this makes it fire -- and makes sure it
# stays quiet on the setext underlines and prose this repository has plenty of. Runs in throwaway git
# repositories, so the real tree is never written to.
set -uo pipefail

real_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failures=0

pass() { printf "  ok   %s\n" "$1"; }
fail() {
	printf "  FAIL %s\n" "$1"
	failures=$((failures + 1))
}

repo_new() {
	local repo
	repo="$(mktemp -d)"
	git -C "$repo" init -q
	mkdir -p "$repo/scripts"
	cp "$real_root/scripts/check_conflict_markers.sh" "$repo/scripts/"
	printf '%s' "$repo"
}

# Stages everything, then runs the guard: true when it says the tree is clean.
clean() {
	git -C "$1" add -A >/dev/null 2>&1
	bash "$1/scripts/check_conflict_markers.sh" >/dev/null 2>&1
}

# The markers are built rather than typed: a fixture holding a real one at the start of a line would be
# reported by the guard running over this repository.
open_marker() { printf '%s HEAD\n' "$(printf '<%.0s' 1 2 3 4 5 6 7)"; }
middle_marker() { printf '%s\n' "$(printf '=%.0s' 1 2 3 4 5 6 7)"; }
close_marker() { printf '%s origin/main\n' "$(printf '>%.0s' 1 2 3 4 5 6 7)"; }

echo "scripts/check_conflict_markers.sh"

repo="$(repo_new)"
printf 'Nothing wrong here.\n' >"$repo/README.md"
if clean "$repo"; then pass "a clean tree passes"; else fail "a clean tree passes"; fi
rm -rf "$repo"

# The file it would most likely happen to.
repo="$(repo_new)"
{
	printf '## [Unreleased]\n\n'
	open_marker
	printf -- "- One branch's entry (#1).\n"
	middle_marker
	printf -- "- The other branch's entry (#2).\n"
	close_marker
} >"$repo/CHANGELOG.md"
if clean "$repo"; then fail "a conflicted CHANGELOG.md fails"; else pass "a conflicted CHANGELOG.md fails"; fi
rm -rf "$repo"

# Any tracked file: the fault is resolving by hand, not the file.
repo="$(repo_new)"
{
	printf '# A comment in a definition file\n'
	open_marker
	printf 'pkt TM { id: 8 = 1; };\n'
	close_marker
} >"$repo/defs.fdsl"
if clean "$repo"; then fail "a conflicted .fdsl fails"; else pass "a conflicted .fdsl fails"; fi
rm -rf "$repo"

repo="$(repo_new)"
printf 'A heading\n=========\n\nAnother\n-------\n\n=======\n' >"$repo/README.md"
if clean "$repo"; then pass "setext underlines are left alone"; else fail "setext underlines are left alone"; fi
rm -rf "$repo"

repo="$(repo_new)"
printf 'Delete the %s and %s lines, then commit.\n' "$(printf '<%.0s' 1 2 3 4 5 6 7)" \
	"$(printf '>%.0s' 1 2 3 4 5 6 7)" >"$repo/notes.md"
printf '%s nope\n' "$(printf '<%.0s' 1 2 3 4 5 6 7 8)" >>"$repo/notes.md"
if clean "$repo"; then
	pass "markers mid-line, or eight long, are not reported"
else
	fail "markers mid-line, or eight long, are not reported"
fi
rm -rf "$repo"

# An untracked file is somebody's scratch copy, not this check's business.
repo="$(repo_new)"
printf 'clean\n' >"$repo/README.md"
git -C "$repo" add -A >/dev/null 2>&1
{
	open_marker
	close_marker
} >"$repo/scratch.md"
if bash "$repo/scripts/check_conflict_markers.sh" >/dev/null 2>&1; then
	pass "an untracked file is not reported"
else
	fail "an untracked file is not reported"
fi
rm -rf "$repo"

# The guard holds the pattern it looks for, and so does this test; neither may report itself.
if bash "$real_root/scripts/check_conflict_markers.sh" >/dev/null 2>&1; then
	pass "this repository is clean, and the guard does not report itself"
else
	fail "this repository is clean, and the guard does not report itself"
fi

if [[ "$failures" -ne 0 ]]; then
	printf "[FAIL] - %d conflict marker check(s) failed\n" "$failures" >&2
	exit 1
fi
echo "The conflict marker guard fires when it should, and only then."
