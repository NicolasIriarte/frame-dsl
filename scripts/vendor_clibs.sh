#!/bin/bash
#
# Updates the CLibs release tarballs committed under third_party/clibs/.
#
# Usage: scripts/vendor_clibs.sh [version]        # default: the version already vendored
#
# This is NOT part of the build. The build unpacks whatever is committed and never reaches the
# network; see the Makefile. Run this by hand, occasionally, to bump CLibs -- then commit the
# tarballs it replaces.
#
# The tarballs are committed rather than fetched per build because fetching needed a credential.
# CLibs is private, so every build and every CI job had to hold a token that could read another
# repository, which made a green build depend on secret configuration rather than on the contents of
# the checkout. 19 KB in git buys that away.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${root}/third_party/clibs"

LIBRARIES=(bash-logger logger testing)

# Fall back to the version already sitting in third_party/clibs, so a plain run refreshes rather
# than silently moving to some default that drifted from what is committed.
current="$(basename "$(ls "${dest}"/logger-*.tar.gz 2>/dev/null | head -n1)" .tar.gz 2>/dev/null || true)"
CLIBS_VERSION="${1:-${current#logger-}}"
CLIBS_VERSION="${CLIBS_VERSION:-v0.1.0}"
CLIBS_URL="${CLIBS_URL:-http://100.125.208.18:3000/nico/CLibs}"

# Stand-ins until bash-logger is available to source; same printf-style calling convention, so no
# call site changes when the real ones take over.
log_info() {
	local fmt="$1"
	shift
	printf "[INFO] - ${fmt}\n" "$@"
}
log_halt() {
	local fmt="$1"
	shift
	printf "[HALT] - ${fmt}\n" "$@" >&2
	exit 1
}

# If a build has already unpacked bash-logger, use the real thing rather than the stand-ins.
if [[ -f "${root}/build/deps/bash-logger/logger.bashrc" ]]; then
	# shellcheck source=/dev/null
	source "${root}/build/deps/bash-logger/logger.bashrc"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# CLibs is private, so updating needs a token -- unlike building, which needs nothing. Read from an
# already exported one, then BYTEWISE_ENV, then this repository's .env.forgejo link, then
# ~/.env.forgejo. Never out of a sibling project's working tree, and never copied into this one.
load_token() {
	if [[ -n "${FORGEJO_TOKEN:-}" ]]; then
		return 0
	fi

	local candidate
	for candidate in "${BYTEWISE_ENV:-}" "${root}/.env.forgejo" "${HOME}/.env.forgejo"; do
		if [[ -n "$candidate" && -f "$candidate" ]]; then
			# shellcheck source=/dev/null
			set -a && source "$candidate" && set +a
			[[ -n "${FORGEJO_TOKEN:-}" ]] && return 0
		fi
	done

	log_halt "no Forgejo token found -- CLibs is private. Run: bytewise --conf-repo ."
}

load_token
mkdir -p "$dest"

for lib in "${LIBRARIES[@]}"; do
	archive="${lib}-${CLIBS_VERSION}.tar.gz"
	url="${CLIBS_URL}/releases/download/${CLIBS_VERSION}/${archive}"

	log_info "fetching %s" "$archive"
	curl -fsSL -H "Authorization: token ${FORGEJO_TOKEN}" -o "${tmp}/${archive}" "$url" \
		|| log_halt "could not download %s" "$url"

	# An unauthenticated request gets a sign-in page under a 200, which curl -f does not catch.
	gzip -t "${tmp}/${archive}" 2>/dev/null \
		|| log_halt "%s did not return an archive -- is the Forgejo token valid?" "$url"

	# Drop any other version of this library first: the Makefile globs, so two would be ambiguous.
	rm -f "${dest}/${lib}"-*.tar.gz
	mv "${tmp}/${archive}" "${dest}/${archive}"
done

log_info "vendored CLibs %s into %s" "$CLIBS_VERSION" "$dest"
log_info "commit the tarballs, and note the bump in CHANGELOG.md"
