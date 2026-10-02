#!/bin/bash
#
# Packs a release's one archive (#128): the tool, the library, its header, the shell completions and
# every editor's support, so that editor support arrives together with the `frame-dsl` every editor
# integration runs. `make dist` is the usual way to run this; it builds what goes in first.
#
# Usage: scripts/build_dist.sh <output.tar.gz> <build-dir> <vsix>
#
# The archive holds one directory named after it. Its layout is `make install`'s under that directory,
# plus what an install leaves out because it is not a system file:
#
#   bin/ lib/ include/ share/bash-completion/ share/zsh/   as `make install PREFIX=<dir>` lays them out
#   lib/libframedsl.a                                       the static archive, which install skips
#   share/frame-dsl/editors/{emacs,vim,vscode}/             each editor's files and README, the .vsix
#   share/frame-dsl/manual/                                 what those READMEs link to
#   README.md CHANGELOG.md INSTALL.md
#
# Each editor's README stays where its links into the manual still resolve; links into the source tree
# (its tests, the vendored client) do not, and are not what a reader of a release needs.
set -euo pipefail

out="${1:?usage: scripts/build_dist.sh <output.tar.gz> <build-dir> <vsix>}"
build="${2:?usage: scripts/build_dist.sh <output.tar.gz> <build-dir> <vsix>}"
vsix="${3:?usage: scripts/build_dist.sh <output.tar.gz> <build-dir> <vsix>}"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$out" in
/*) : ;;
*) out="$(pwd)/${out}" ;;
esac
name="$(basename "$out" .tar.gz)"

log_halt() {
	printf "[HALT] - %s\n" "$*" >&2
	exit 1
}

[ -f "${build}/libframedsl.a" ] || log_halt "${build}/libframedsl.a is missing -- run make lib"
[ -f "$vsix" ] || log_halt "${vsix} is missing -- run make vsix"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
stage="${tmp}/${name}"
mkdir -p "$stage"

# The install layout, from the install target itself, so the two cannot drift apart.
make -C "$root" --no-print-directory install DESTDIR="$stage" PREFIX= >/dev/null
install -m 644 "${build}/libframedsl.a" "${stage}/lib/"

editors="${stage}/share/frame-dsl/editors"
mkdir -p "${editors}/emacs" "${editors}/vim" "${editors}/vscode"
install -m 644 "${root}/editors/README.md" "${editors}/"
install -m 644 "${root}/editors/emacs/frame-dsl-mode.el" "${root}/editors/emacs/README.md" "${editors}/emacs/"
# The Vim runtime directory whole, less its test script.
(cd "${root}/editors/vim" && find . -type f ! -name 'frame-dsl-tests.vim' -print0) \
	| (cd "${root}/editors/vim" && xargs -0 -I{} install -D -m 644 {} "${editors}/vim/{}")
install -m 644 "$vsix" "${root}/editors/vscode/README.md" "${editors}/vscode/"

mkdir -p "${stage}/share/frame-dsl/manual"
install -m 644 "${root}"/manual/*.md "${stage}/share/frame-dsl/manual/"
install -m 644 "${root}/README.md" "${root}/CHANGELOG.md" "$stage/"
install -m 644 "${root}/scripts/dist_install.md" "${stage}/INSTALL.md"

# Owned by nobody in particular, and in a stable order, so the archive is the same each time it is
# built from the same files.
mkdir -p "$(dirname "$out")"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@0" -C "$tmp" -czf "$out" "$name"
