#!/bin/bash
#
# Fails if any tracked file holds an unresolved merge conflict marker.
#
# Usage: scripts/check_conflict_markers.sh [path...]
#          scripts/check_conflict_markers.sh          # everything git tracks
#
# Exits 0 if the tree is clean, 1 having listed what it found.
#
# A marker in a C file breaks the build loudly; one in Markdown, JSON or an .fdsl comment is silent until
# somebody reads it. CHANGELOG.md was hand-merged on nearly every pull request before #90, and NicOs had
# one of those hand merges reach main with its markers still in it, with nothing to notice. The fault is
# resolving a conflict by hand, not the file, so this looks at every tracked file.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

# The opening and closing markers, anchored to the start of a line and to exactly seven characters.
# `=======` alone is deliberately not looked for: it is a Markdown setext underline. The other two are
# enough to catch a real conflict, and neither has an innocent meaning at the start of a line.
MARKERS='^(<{7}|>{7})( |$)'

if [[ $# -gt 0 ]]; then
	mapfile -t files < <(git ls-files -- "$@")
else
	mapfile -t files < <(git ls-files)
fi

# No exclusion list, not even for this file or its test: neither holds a marker at the start of a line
# -- the pattern is a regex, and the test builds its fixtures with printf. An exclusion list starts at
# one file and ends as the first place anyone adds a name to make a red check go away.
found=0
for file in "${files[@]}"; do
	# Binary files are skipped: a tarball can hold those bytes by chance, and a report about it would be
	# a red check nobody can act on.
	if [[ ! -f "$file" ]] || ! grep -Iq . "$file" 2>/dev/null; then
		continue
	fi
	if hits="$(grep -nE "$MARKERS" "$file" 2>/dev/null)"; then
		printf "%s:\n" "$file" >&2
		sed 's/^/    /' <<<"$hits" >&2
		found=$((found + 1))
	fi
done

if [[ "$found" -ne 0 ]]; then
	printf "[FAIL] - %d tracked file(s) still hold merge conflict markers\n" "$found" >&2
	exit 1
fi
echo "No conflict markers in any tracked file."
