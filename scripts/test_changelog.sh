#!/bin/bash
#
# Checks the changelog scripts do what changelog.d/README.md says (#90), without a tag or a pull request.
#
# Usage: scripts/test_changelog.sh      (`make test` runs it)
#
# A release happens rarely enough that the assembler would otherwise run for the first time in the
# middle of cutting one, which is the worst moment to learn it drops an entry; and a CI gate that quietly
# never fires looks exactly like one that passes. Everything runs in a throwaway directory, so the real
# CHANGELOG.md and changelog.d/ are never touched.
set -uo pipefail

real_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
failures=0

pass() { printf "  ok   %s\n" "$1"; }
fail() {
	printf "  FAIL %s\n" "$1"
	failures=$((failures + 1))
}
expect() { # expect <name> <command...>: passes when the command succeeds
	local name="$1"
	shift
	if "$@" >/dev/null 2>&1; then pass "$name"; else fail "$name"; fi
}
refuse() { # refuse <name> <command...>: passes when the command fails
	local name="$1"
	shift
	if "$@" >/dev/null 2>&1; then fail "$name"; else pass "$name"; fi
}

# A project root holding nothing but what the scripts read.
sandbox() {
	local box
	box="$(mktemp -d)"
	mkdir -p "$box/scripts" "$box/changelog.d"
	cp "$real_root/scripts/changelog_assemble.sh" "$real_root/scripts/changelog_section.sh" "$box/scripts/"
	cat >"$box/CHANGELOG.md" <<-'EOF'
		# Changelog

		Preamble that must survive untouched.

		## [Unreleased]

		## [0.1.0] - 2026-09-01

		### Fixed

		- Something that was already released.
	EOF
	printf '%s' "$box"
}

fragment() { # fragment <box> <section> <file> <text>
	mkdir -p "$1/changelog.d/$2"
	printf '%s\n' "$4" >"$1/changelog.d/$2/$3"
}

assemble() { # assemble <box> <args...>
	local box="$1"
	shift
	bash "$box/scripts/changelog_assemble.sh" "$@"
}

echo "scripts/changelog_assemble.sh"

# ---- Grouping, heading order and the entries themselves -------------------------------------------------
box="$(sandbox)"
fragment "$box" fixed "73-quick-fixes.md" "- A quick fix no longer eats the line after it (#73)."
fragment "$box" added "72-hover.md" "- Hover shows where a field lands (#72)."
fragment "$box" changed "85-completion.md" "- \`--check\` prints \`%s\\n\` as written (#85)."
fragment "$box" fixed "70-diagnostics.md" "- A diagnostic spans its whole token (#70)."
got="$(assemble "$box" v0.2.0 --preview --date 2026-09-24)"

# Written out as a person would write it, and compared byte for byte -- the backslash included, which an
# awk -v would have turned into a newline.
want="$(
	cat <<-'EOF'
		## [0.2.0] - 2026-09-24

		### Added

		- Hover shows where a field lands (#72).

		### Changed

		- `--check` prints `%s\n` as written (#85).

		### Fixed

		- A diagnostic spans its whole token (#70).
		- A quick fix no longer eats the line after it (#73).
	EOF
)"
if [[ "$got" == "$want" ]]; then
	pass "a section is byte-identical to a hand-written one"
else
	fail "a section is byte-identical to a hand-written one"
	diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/       /'
fi

# The preview wrote nothing.
expect "a preview changes nothing" test -f "$box/changelog.d/fixed/73-quick-fixes.md"

# And written for real, the backslash survives into the file too.
assemble "$box" v0.2.0 --date 2026-09-24 >/dev/null 2>&1
expect "the file keeps a backslash as written" grep -qF '`%s\n` as written' "$box/CHANGELOG.md"
rm -rf "$box"

# ---- Within a section, by issue number, not by merge order or by string ---------------------------------
box="$(sandbox)"
fragment "$box" fixed "312-later.md" "- Later issue."
fragment "$box" fixed "99-earlier.md" "- Earlier issue."
got="$(assemble "$box" v0.2.0 --preview)"
if [[ "$(grep -n 'Earlier' <<<"$got" | cut -d: -f1)" -lt "$(grep -n 'Later' <<<"$got" | cut -d: -f1)" ]]; then
	pass "entries are ordered by issue number, not lexically"
else
	fail "entries are ordered by issue number, not lexically"
fi
rm -rf "$box"

# ---- Written in for real ------------------------------------------------------------------------------
box="$(sandbox)"
fragment "$box" fixed "73-quick-fixes.md" "- A quick fix (#73)."
assemble "$box" v0.2.0 --date 2026-09-24 >/dev/null 2>&1
expect "the version's heading is written" grep -q '^## \[0\.2\.0\] - 2026-09-24$' "$box/CHANGELOG.md"
if [[ "$(grep -n '^## \[Unreleased\]$' "$box/CHANGELOG.md" | cut -d: -f1)" \
	-lt "$(grep -n '^## \[0\.2\.0\]' "$box/CHANGELOG.md" | cut -d: -f1)" ]]; then
	pass "a fresh, empty [Unreleased] is left above it"
else
	fail "a fresh, empty [Unreleased] is left above it"
fi
if grep -q '^Preamble that must survive untouched\.$' "$box/CHANGELOG.md" \
	&& grep -q '^- Something that was already released\.$' "$box/CHANGELOG.md"; then
	pass "the preamble and older releases are untouched"
else
	fail "the preamble and older releases are untouched"
fi
refuse "the fragments it used are removed" test -e "$box/changelog.d/fixed/73-quick-fixes.md"
refuse "an emptied section directory is removed too" test -d "$box/changelog.d/fixed"
# The blank line that used to separate [Unreleased] from the release below goes with the old section;
# without putting one back, the last entry butts against the next heading.
if awk '
	/^## \[0\.2\.0\]/ { inside = 1; next }
	inside && /^## \[/ { exit previous == "" ? 0 : 1 }
	inside { previous = $0 }
' "$box/CHANGELOG.md"; then
	pass "a blank line separates the new section from the one below"
else
	fail "a blank line separates the new section from the one below"
fi
refuse "a version already in the file is refused" assemble "$box" v0.2.0
rm -rf "$box"

# ---- Entries written the old way, straight under [Unreleased] -------------------------------------------
box="$(sandbox)"
sed -i 's/^## \[Unreleased\]$/## [Unreleased]\n\n### Added\n\n- Written the old way (#1)./' "$box/CHANGELOG.md"
fragment "$box" fixed "73-quick-fixes.md" "- A quick fix (#73)."
got="$(assemble "$box" v0.2.0 --preview)"
if [[ "$got" == *"Written the old way"* && "$got" == *"A quick fix"* ]] \
	&& [[ "$(grep -n 'old way' <<<"$got" | cut -d: -f1)" -lt "$(grep -n 'quick fix' <<<"$got" | cut -d: -f1)" ]]; then
	pass "leftover [Unreleased] entries are carried in, ahead of the fragments"
else
	fail "leftover [Unreleased] entries are carried in, ahead of the fragments"
fi
assemble "$box" v0.2.0 >/dev/null 2>&1
if awk '/^## \[Unreleased\]/{t=1;next} /^## \[/{t=0} t&&/old way/{found=1} END{exit found}' "$box/CHANGELOG.md" \
	&& grep -q 'Written the old way' "$box/CHANGELOG.md"; then
	pass "and are moved out of [Unreleased], not copied"
else
	fail "and are moved out of [Unreleased], not copied"
fi
rm -rf "$box"

# One `### Added`, not one from [Unreleased] and another from the fragments; a heading Keep a Changelog
# does not have is kept, after the rest.
box="$(sandbox)"
sed -i 's/^## \[Unreleased\]$/## [Unreleased]\n\n### Added\n\n- Old addition (#1).\n\n### Notes\n\nA remark./' \
	"$box/CHANGELOG.md"
fragment "$box" added "72-hover.md" "- New addition (#72)."
fragment "$box" fixed "73-fix.md" "- New fix (#73)."
got="$(assemble "$box" v0.2.0 --preview --date 2026-09-24)"
want="$(
	cat <<-'EOF'
		## [0.2.0] - 2026-09-24

		### Added

		- Old addition (#1).
		- New addition (#72).

		### Fixed

		- New fix (#73).

		### Notes

		A remark.
	EOF
)"
if [[ "$got" == "$want" ]]; then
	pass "[Unreleased]'s headings and the fragments' are merged, not repeated"
else
	fail "[Unreleased]'s headings and the fragments' are merged, not repeated"
	diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | sed 's/^/       /'
fi
rm -rf "$box"

# ---- What is refused -----------------------------------------------------------------------------------
box="$(sandbox)"
refuse "a release with nothing to say is refused" assemble "$box" v0.2.0 --preview
rm -rf "$box"

box="$(sandbox)"
fragment "$box" fixed "73-fine.md" "- Fine."
fragment "$box" improved "74-nope.md" "- In a section Keep a Changelog does not have."
refuse "a fragment in an unknown section is refused" assemble "$box" v0.2.0 --preview
rm -rf "$box"

box="$(sandbox)"
fragment "$box" fixed "73-no-bullet.md" "A quick fix (#73)."
refuse "a fragment without a leading '- ' is refused" assemble "$box" v0.2.0 --preview
rm -rf "$box"

box="$(sandbox)"
mkdir -p "$box/changelog.d/fixed"
: >"$box/changelog.d/fixed/73-empty.md"
refuse "an empty fragment is refused" assemble "$box" v0.2.0 --preview
rm -rf "$box"

box="$(sandbox)"
fragment "$box" "" "73-loose.md" "- Not in a section."
refuse "a fragment outside any section is refused" assemble "$box" v0.2.0 --preview
rm -rf "$box"

box="$(sandbox)"
printf '# Changelog fragments\n' >"$box/changelog.d/README.md"
fragment "$box" fixed "73-fine.md" "- Fine."
expect "changelog.d/README.md is not taken for an entry" assemble "$box" v0.2.0 --preview
rm -rf "$box"

# ---- The point of all this: two changes never touch the same lines --------------------------------------
#
# With git merge-tree, which merges with no working tree. That is how Forgejo merges, and why
# .gitattributes' merge=union alone was not enough: a merge driver is only applied when .gitattributes is
# a real file in the tree doing the merge.
merges_cleanly() { git -C "$1" merge-tree --write-tree "$2" "$3" >/dev/null 2>&1; }

repo="$(mktemp -d)"
git -C "$repo" init -q
git -C "$repo" config user.email t@example.com
git -C "$repo" config user.name t
cp "$real_root/CHANGELOG.md" "$repo/CHANGELOG.md"
git -C "$repo" add -A && git -C "$repo" commit -qm base
base="$(git -C "$repo" rev-parse --abbrev-ref HEAD)"

for n in 1 2; do
	git -C "$repo" checkout -q -b "b$n" "$base"
	mkdir -p "$repo/changelog.d/added"
	printf -- '- Change %s (#%s).\n' "$n" "$n" >"$repo/changelog.d/added/$n-change.md"
	git -C "$repo" add -A && git -C "$repo" commit -qm "b$n"
done
# Asserted rather than assumed: a setup that failed to commit a fragment would leave nothing to conflict,
# and the check below would pass for the wrong reason.
if git -C "$repo" cat-file -e b1:changelog.d/added/1-change.md 2>/dev/null \
	&& git -C "$repo" cat-file -e b2:changelog.d/added/2-change.md 2>/dev/null; then
	pass "both branches really did add a fragment"
else
	fail "both branches really did add a fragment"
fi
if merges_cleanly "$repo" b1 b2 && merges_cleanly "$repo" b2 b1; then
	pass "two branches adding fragments merge in either order, with no working tree"
else
	fail "two branches adding fragments merge in either order, with no working tree"
fi

# And the shape it replaces, to show the check can tell the difference.
for n in 1 2; do
	git -C "$repo" checkout -q "b$n"
	sed -i "s/^## \[Unreleased\]\$/## [Unreleased]\n\n### Added\n\n- Change $n./" "$repo/CHANGELOG.md"
	git -C "$repo" add -A && git -C "$repo" commit -qm "b$n edits CHANGELOG.md"
done
if merges_cleanly "$repo" b1 b2; then
	fail "two branches editing [Unreleased] conflict, so the check can tell the difference"
else
	pass "two branches editing [Unreleased] conflict, so the check can tell the difference"
fi
rm -rf "$repo"

echo "scripts/changelog_section.sh"

box="$(sandbox)"
fragment "$box" added "72-hover.md" "- Hover (#72)."
assemble "$box" v0.2.0 --date 2026-09-24 >/dev/null 2>&1
notes="$(bash "$box/scripts/changelog_section.sh" v0.2.0 "$box/CHANGELOG.md" 2>/dev/null)"
if [[ "$notes" == $'### Added\n\n- Hover (#72).' ]]; then
	pass "a version's notes are read back, without blank lines at either end"
else
	fail "a version's notes are read back, without blank lines at either end"
	printf '%s\n' "$notes" | sed 's/^/       /'
fi
expect "the version may be written without its v" bash "$box/scripts/changelog_section.sh" 0.1.0 "$box/CHANGELOG.md"
refuse "a version with no section is refused" bash "$box/scripts/changelog_section.sh" v9.9.9 "$box/CHANGELOG.md"
# [Unreleased] is a section with a heading and nothing under it, which is exactly what a tag cut before
# assembling would find.
refuse "an empty section is refused" bash "$box/scripts/changelog_section.sh" Unreleased "$box/CHANGELOG.md"
rm -rf "$box"

echo "scripts/changelog_required.sh"

# required <name> <owed|settled> <path...>
required() {
	local name="$1" want="$2"
	shift 2
	local got=settled
	if printf '%s\n' "$@" | bash "$real_root/scripts/changelog_required.sh" >/dev/null 2>&1; then
		got=owed
	fi
	if [[ "$got" == "$want" ]]; then pass "$name -> $got"; else fail "$name -> $got, expected $want"; fi
}

# What a user would notice, changed without a word.
required "the library" owed src/parser.c
required "the public header" owed include/framedsl.h
required "the editor support" owed editors/frame-dsl-mode.el
required "a source file among others" owed README.md test/unit/2_parser.c src/lsp.c

# The same, with a fragment of its own.
required "the library, with a fragment" settled src/parser.c changelog.d/added/73-quick-fixes.md
required "one change, several entries" settled src/lsp.c changelog.d/added/73-a.md changelog.d/fixed/73-b.md
required "every section is accepted" settled src/diag.c changelog.d/deprecated/1-a.md
required "security too" settled src/diag.c changelog.d/security/1-a.md

# CHANGELOG.md is assembled at release; editing it instead is still owed a fragment.
required "CHANGELOG.md edited instead" owed src/parser.c CHANGELOG.md

# Near misses, each an entry nobody would ever publish -- told on the pull request, not at the release.
required "a fragment in no section" owed src/parser.c changelog.d/73-quick-fixes.md
required "a fragment in an unknown section" owed src/parser.c changelog.d/improved/73-quick-fixes.md
required "a fragment that is not Markdown" owed src/parser.c changelog.d/added/73-quick-fixes.txt
required "a fragment one level too deep" owed src/parser.c changelog.d/added/73/quick-fixes.md
required "the fragment README alone" owed src/parser.c changelog.d/README.md

# Work on the project rather than on what it ships.
required "the manual" settled manual/editors.md
required "an example" settled examples/uart_device.fdsl
required "a test" settled test/system/18_quick_fixes.c
required "a script" settled scripts/build_vsix.sh
required "the workflow" settled .forgejo/workflows/ci.yml
required "the Makefile" settled Makefile
required "a fragment alone" settled changelog.d/fixed/73-quick-fixes.md
required "an empty diff" settled ""

if [[ "$failures" -ne 0 ]]; then
	printf "[FAIL] - %d changelog check(s) failed\n" "$failures" >&2
	exit 1
fi
echo "The changelog scripts do what they say."
