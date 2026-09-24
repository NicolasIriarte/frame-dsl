#!/bin/bash
#
# Says whether a set of changed files owes the changelog an entry it does not have.
#
# Usage: git diff --name-only BASE HEAD | scripts/changelog_required.sh
#
# Exits 0 if an entry is owed and missing, having said what to add; 1 if nothing is owed.
#
# A script rather than a condition inside .forgejo/workflows/ci.yml, so the decision can be checked
# without opening a pull request to watch what CI does. scripts/test_changelog.sh runs it against the
# cases that matter. There is deliberately no opt-out: a label or a marker in the description is a thing
# to remember, then to reach for, then one nobody notices being used. The path list is the decision.
set -euo pipefail

# What somebody using Frame-DSL would notice (#90): the library and the tool (src/), the public header
# (include/), and the editor support (editors/). Everything else -- the manual, examples, tests, scripts,
# CI -- is work on the project rather than on what it ships, and demanding an entry for every test fix
# would train everyone to write entries that say nothing.
RECORDED_PATHS='^(src/|include/|editors/)'

# What settles it: a fragment of its own, not a line in a file every other pull request is also editing.
FRAGMENT_PATHS='^changelog\.d/(added|changed|deprecated|removed|fixed|security)/[^/]+\.md$'

changed="$(cat)"

if [[ -z "${changed//[[:space:]]/}" ]]; then
	echo "No changed files given; nothing to record."
	exit 1
fi

if ! grep -qE "$RECORDED_PATHS" <<<"$changed"; then
	echo "Nothing a user would notice changed."
	exit 1
fi

if grep -qE "$FRAGMENT_PATHS" <<<"$changed"; then
	echo "A changelog fragment was added alongside:"
	grep -E "$RECORDED_PATHS" <<<"$changed" | sed 's/^/  /'
	exit 1
fi

echo "These changed with no changelog fragment to go with them:"
grep -E "$RECORDED_PATHS" <<<"$changed" | sed 's/^/  /'
cat <<'EOF'

Add one file per entry, in the same change that makes it:

  changelog.d/<added|changed|deprecated|removed|fixed|security>/<issue>-<slug>.md

It holds the entry as it should read, bullet and all. See changelog.d/README.md.

Editing CHANGELOG.md directly does not count: that file is assembled when a version is cut, and every
pull request editing the same lines of it is what #90 was about.
EOF
exit 0
