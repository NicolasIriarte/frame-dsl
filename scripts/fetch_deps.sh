#!/bin/bash
#
# Fetches the CLibs libraries this project builds against into third_party/.
#
# Usage: scripts/fetch_deps.sh
#
# The libraries are downloaded rather than vendored or submoduled: a version bump is then the one
# variable below, and this repository does not carry a copy of someone else's source that has to be
# re-vendored by hand. third_party/ is gitignored -- nothing here is committed.
#
# Each library records the version it was unpacked at, so a second run costs three file reads, and
# changing CLIBS_VERSION re-fetches exactly the libraries that are now out of date.
#
# CLibs is a private repository, so this needs the same Forgejo token the rest of the project uses.
# See GUIDELINES.md: `bytewise --conf-repo .` links it in.
set -euo pipefail

CLIBS_VERSION="${CLIBS_VERSION:-v0.1.0}"
CLIBS_URL="${CLIBS_URL:-http://100.125.208.18:3000/nico/CLibs}"

LIBRARIES=(bash-logger logger testing)

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
dest="${root}/third_party"

# bash-logger is fetched first and then sourced, so every line after that point reports in the same
# shape the C side does. Until it is on disk there is nothing to report with, so these two stand in.
# They take a printf-style format like the real ones, so no call site changes when they are replaced.
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

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Read the token from wherever this checkout keeps it, preferring an already-exported one. The file
# is never read out of a sibling project's working tree, and never copied into this one.
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

fetch_one() {
	local lib="$1"
	local target="${dest}/${lib}"
	local stamp="${target}/.version"

	if [[ -f "$stamp" && "$(<"$stamp")" == "$CLIBS_VERSION" ]]; then
		log_info "%s is already at %s" "$lib" "$CLIBS_VERSION"
		return 0
	fi

	local url="${CLIBS_URL}/releases/download/${CLIBS_VERSION}/${lib}-${CLIBS_VERSION}.tar.gz"
	local archive="${tmp}/${lib}.tar.gz"

	log_info "fetching %s %s" "$lib" "$CLIBS_VERSION"
	curl -fsSL -H "Authorization: token ${FORGEJO_TOKEN}" -o "$archive" "$url" \
		|| log_halt "could not download %s" "$url"

	# An unauthenticated or expired token gets a sign-in page with a 200 on it, which -f does not
	# catch. Checking the archive is really an archive turns that into one clear line instead of a
	# gzip error from inside tar.
	gzip -t "$archive" 2>/dev/null \
		|| log_halt "%s did not return an archive -- is the Forgejo token valid?" "$url"

	# The tarball holds a single <lib>-<version>/ directory. That component is stripped so the
	# version lives in the stamp file alone, and no include path has to be edited to bump it.
	rm -rf "$target"
	mkdir -p "$target"
	tar xzf "${tmp}/${lib}.tar.gz" -C "$target" --strip-components=1
	printf '%s\n' "$CLIBS_VERSION" >"$stamp"
}

mkdir -p "$dest"
load_token

fetch_one bash-logger
# shellcheck source=/dev/null
source "${dest}/bash-logger/logger.bashrc"

for lib in "${LIBRARIES[@]}"; do
	[[ "$lib" == "bash-logger" ]] && continue
	fetch_one "$lib"
done

# Written last and only on success: the Makefile treats this as the marker that third_party/ is
# usable, and depends on this script itself, so editing CLIBS_VERSION above is enough to re-run.
printf '%s\n' "$CLIBS_VERSION" >"${dest}/.fetched"
log_info "CLibs %s ready in %s" "$CLIBS_VERSION" "$dest"
