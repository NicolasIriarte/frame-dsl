#!/bin/bash
#
# Prints the CHANGELOG.md section for one version: the notes a release is published with.
#
# Usage: scripts/changelog_section.sh <version> [changelog-path]
#          scripts/changelog_section.sh v0.1.0
#
# Exits 0 having printed the section, or 1 having printed nothing if that version has no section or an
# empty one.
#
# Refusing is the point. A release with nothing written about it is one nobody can decide whether to
# install, and nobody goes back to fill it in -- so a tag whose version was never written down fails the
# release job here, before anything is published (#90).
set -euo pipefail

version="${1:?usage: scripts/changelog_section.sh <version> [changelog-path]}"
changelog="${2:-CHANGELOG.md}"

if [[ ! -f "$changelog" ]]; then
	printf "changelog_section: %s does not exist\n" "$changelog" >&2
	exit 1
fi

# Tags are written v0.1.0; the heading is `## [0.1.0] - <date>`. Either spelling is taken.
wanted="${version#v}"

section="$(awk -v wanted="$wanted" '
	/^## \[/ {
		name = $0
		sub(/^## \[/, "", name)
		sub(/\].*$/, "", name)
		if (name == wanted) { printing = 1; next }
		if (printing) { exit }
		next
	}
	printing { print }
' "$changelog")"

if [[ -z "${section//[[:space:]]/}" ]]; then
	printf "changelog_section: no section for version %s in %s\n" "$wanted" "$changelog" >&2
	exit 1
fi

# Without the blank lines a section has at either end, so the release body does not open with them.
printf '%s' "$section" | sed -e '/./,$!d' -e :a -e '/^\n*$/{$d;N;ba' -e '}'
printf '\n'
