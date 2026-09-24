#!/bin/bash
#
# Folds the fragments under changelog.d/ into a version's section of CHANGELOG.md.
#
# Usage: scripts/changelog_assemble.sh <version> [--preview] [--date YYYY-MM-DD]
#          scripts/changelog_assemble.sh v0.1.0            # rewrites CHANGELOG.md, removes the fragments
#          scripts/changelog_assemble.sh v0.1.0 --preview  # prints the section, changes nothing
#
# `make changelog VERSION=v0.1.0` and `make changelog-preview` are the usual ways in.
#
# Exits 0 having done it, or 1 having said why not.
#
# The other half of changelog.d/ existing at all (#90): entries are written one per file so that no two
# pull requests edit the same lines, and something has to put them back together when a version is cut.
# A script rather than a step in the tag issue's checklist, because a release happens rarely enough that
# assembling one by hand would be re-learnt each time and got wrong once. scripts/test_changelog.sh
# checks it without cutting a tag.
#
# Assembly is concatenation. Nothing here re-wraps, re-indents or re-orders the text inside a fragment,
# so what was reviewed in the pull request is byte for byte what the release notes say.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fragment_dir="${root}/changelog.d"
changelog="${root}/CHANGELOG.md"

log_fail() {
	printf "[FAIL] - %s\n" "$*" >&2
}

log_halt() {
	printf "[HALT] - %s\n" "$*" >&2
	exit 1
}

# The Keep a Changelog headings, in the order that standard prints them. Fixed here rather than taken
# from whichever directories exist, so every release's sections come out in the same order.
SECTIONS=(added changed deprecated removed fixed security)

heading_for() {
	case "$1" in
	added) echo "Added" ;;
	changed) echo "Changed" ;;
	deprecated) echo "Deprecated" ;;
	removed) echo "Removed" ;;
	fixed) echo "Fixed" ;;
	security) echo "Security" ;;
	*) return 1 ;;
	esac
}

version=""
preview=0
date="$(date +%Y-%m-%d)"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--preview)
		preview=1
		shift
		;;
	--date)
		date="${2:?--date needs a YYYY-MM-DD}"
		shift 2
		;;
	-*)
		log_halt "unknown option '$1'"
		;;
	*)
		version="$1"
		shift
		;;
	esac
done

[[ -n "$version" ]] || log_halt "usage: scripts/changelog_assemble.sh <version> [--preview] [--date YYYY-MM-DD]"

# Tags are written v0.1.0 and the heading is `## [0.1.0] - <date>`. Either spelling is taken, as
# scripts/changelog_section.sh does, so a caller never has to remember which side of that it is on.
wanted="${version#v}"

[[ -f "$changelog" ]] || log_halt "${changelog} does not exist"

# Anything under changelog.d/ that is not a usable fragment.
#
# Refused rather than skipped: a file in the wrong place is an entry nobody will ever publish, found as a
# gap in the release notes long after the change it described was merged -- the failure this whole
# arrangement exists to avoid.
check_strays() {
	local strays=0
	local path name section leaf

	while IFS= read -r path; do
		[[ -n "$path" ]] || continue
		name="${path#"$fragment_dir"/}"
		[[ "$name" == "README.md" ]] && continue

		section="${name%%/*}"
		leaf="${name#*/}"

		if [[ "$name" != */* ]] || ! heading_for "$section" >/dev/null; then
			log_fail "  ${name} is not in one of: ${SECTIONS[*]}"
			strays=$((strays + 1))
		elif [[ "$leaf" != *.md || "$leaf" == */* ]]; then
			log_fail "  ${name} should be <issue>-<slug>.md directly under ${section}/"
			strays=$((strays + 1))
		elif [[ ! -s "$path" ]]; then
			log_fail "  ${name} is empty"
			strays=$((strays + 1))
		elif ! head -n 1 "$path" | grep -q '^- '; then
			# The bullet is the fragment's own, so one that forgot it would land as a stray paragraph
			# in the middle of a list.
			log_fail "  ${name} does not start with '- '; see changelog.d/README.md"
			strays=$((strays + 1))
		fi
	done < <(find "$fragment_dir" -type f 2>/dev/null | sort)

	[[ "$strays" -eq 0 ]] || log_halt "${strays} file(s) under changelog.d/ are not usable entries"
}

# Every fragment of one section, lowest issue number first -- so a section reads the same however its
# branches happened to merge. `sort -V` on the file's own name, so 99 comes before 312.
fragments_of() {
	local section="$1"
	[[ -d "${fragment_dir}/${section}" ]] || return 0
	find "${fragment_dir}/${section}" -maxdepth 1 -type f -name '*.md' -printf '%f\t%p\n' | sort -V | cut -f2-
}

# Blank lines off both ends.
trim() {
	sed -e '/./,$!d' -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

check_strays

# Whatever is still under `## [Unreleased]`, written before fragments existed or by a branch that did not
# know about them. Carried into the release rather than dropped: those entries are as much a part of it,
# and losing them on the one occasion it mattered would be worse than the conflicts this replaced.
unreleased="$(awk '
	/^## \[Unreleased\]/ { taking = 1; next }
	/^## \[/             { if (taking) exit }
	taking               { print }
' "$changelog")"

# The part of it under `### <heading>` -- or, for "", what comes before the first heading.
unreleased_under() {
	HEADING="$1" awk '
		/^### / { current = substr($0, 5); next }
		current == ENVIRON["HEADING"] { print }
	' <<<"$unreleased" | trim
}

# Any `### ` block of it whose heading is not a Keep a Changelog one, heading included.
unreleased_other() {
	awk '
		BEGIN { split("Added Changed Deprecated Removed Fixed Security", k, " "); for (i in k) known[k[i]] = 1 }
		/^### / { other = !(substr($0, 5) in known) }
		other { print }
	' <<<"$unreleased" | trim
}

# The version's section: under each heading, in Keep a Changelog order, what [Unreleased] already had and
# then the fragments -- one `### Added`, however the entries under it were written. The older entries
# first, so a reader meets the work in the order it happened.
section_text() {
	local out part section heading entries path
	out="$(unreleased_under "")"
	for section in "${SECTIONS[@]}"; do
		heading="$(heading_for "$section")"
		entries="$(unreleased_under "$heading")"
		while IFS= read -r path; do
			[[ -n "$path" ]] || continue
			# Trimmed, so entries sit one after another however the file was saved.
			part="$(trim <"$path")"
			entries="${entries:+${entries}$'\n'}${part}"
		done < <(fragments_of "$section")
		if [[ -n "${entries//[[:space:]]/}" ]]; then
			out="${out:+${out}$'\n\n'}### ${heading}"$'\n\n'"${entries}"
		fi
	done
	part="$(unreleased_other)"
	if [[ -n "${part//[[:space:]]/}" ]]; then
		out="${out:+${out}$'\n\n'}${part}"
	fi
	printf '%s' "$out"
}

section="$(section_text)"

if [[ -z "${section//[[:space:]]/}" ]]; then
	log_halt "nothing to release: no fragments under changelog.d/ and nothing under [Unreleased]"
fi

if [[ "$preview" -eq 1 ]]; then
	printf '## [%s] - %s\n\n%s\n' "$wanted" "$date" "$section"
	exit 0
fi

if grep -qE "^## \[${wanted//./\\.}\]" "$changelog"; then
	log_halt "CHANGELOG.md already has a section for ${wanted}"
fi

# Through a temporary file moved into place, so an interrupted run cannot leave half a changelog behind.
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

# The section reaches awk through the environment, not -v: -v interprets backslash escapes, and an entry
# quoting `\n` in a format string would come out with a newline in it.
SECTION="$section" awk -v wanted="$wanted" -v when="$date" '
	/^## \[Unreleased\]/ {
		# A fresh, empty [Unreleased] stays on top, then the version being cut. Two newlines after the
		# section: the blank line that used to separate [Unreleased] from the release below it went
		# with the rest of the old section.
		print "## [Unreleased]"
		print ""
		printf "## [%s] - %s\n\n%s\n\n", wanted, when, ENVIRON["SECTION"]
		taking = 1
		next
	}
	taking && /^## \[/ { taking = 0 }
	taking { next }
	{ print }
' "$changelog" >"$tmp"

mv "$tmp" "$changelog"
trap - EXIT
printf "Wrote the %s section of CHANGELOG.md.\n" "$wanted"

# Removed only once they are safely in the file. A fragment left behind would be published a second time
# with the next release.
removed=0
for section_name in "${SECTIONS[@]}"; do
	while IFS= read -r path; do
		[[ -n "$path" ]] || continue
		rm -f "$path"
		removed=$((removed + 1))
	done < <(fragments_of "$section_name")
	# An emptied section directory goes too, so `ls changelog.d/` after a release says nothing is pending.
	if [[ -d "${fragment_dir}/${section_name}" ]]; then
		rmdir "${fragment_dir}/${section_name}" 2>/dev/null || true
	fi
done
printf "Consumed %d fragment(s).\n" "$removed"
