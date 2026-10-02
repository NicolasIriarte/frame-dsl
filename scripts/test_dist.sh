#!/bin/bash
#
# Checks a release archive from the outside (#128): unpacked somewhere else entirely, it holds every
# file the release promises, its tool runs and loads a definition, and its header and library build a
# program -- both ways, static and shared. `make test-dist` builds the archive and runs this.
#
# Usage: scripts/test_dist.sh <archive.tar.gz> [version]
#
# With a version, the tool must say that is what it is: the release job passes the tag, so a release
# whose binary reports a commit hash instead is refused rather than published.
set -uo pipefail

archive="${1:?usage: scripts/test_dist.sh <archive.tar.gz> [version]}"
version="${2:-}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
tar -xzf "$archive" -C "$tmp" || {
	echo "cannot unpack $archive"
	exit 1
}
name="$(basename "$archive" .tar.gz)"
here="${tmp}/${name}"
expect "the archive holds one directory, named after it" test -d "$here"
expect "and nothing beside it" test "$(ls "$tmp" | wc -l)" -eq 1

for file in bin/frame-dsl lib/libframedsl.a lib/libframedsl.so lib/libframedsl.so.0 include/framedsl.h \
	share/bash-completion/completions/frame-dsl share/zsh/site-functions/_frame-dsl \
	share/frame-dsl/editors/README.md \
	share/frame-dsl/editors/emacs/frame-dsl-mode.el share/frame-dsl/editors/emacs/README.md \
	share/frame-dsl/editors/vim/README.md share/frame-dsl/editors/vim/ftdetect/fdsl.vim \
	share/frame-dsl/editors/vim/ftplugin/fdsl.vim share/frame-dsl/editors/vim/syntax/fdsl.vim \
	share/frame-dsl/editors/vim/indent/fdsl.vim share/frame-dsl/editors/vim/compiler/fdsl.vim \
	share/frame-dsl/editors/vim/lsp/frame_dsl.lua share/frame-dsl/editors/vscode/README.md \
	share/frame-dsl/manual/language.md share/frame-dsl/manual/api.md share/frame-dsl/manual/editors.md \
	README.md CHANGELOG.md INSTALL.md; do
	expect "holds ${file}" test -e "${here}/${file}"
done
expect "holds the VS Code extension" compgen -G "${here}/share/frame-dsl/editors/vscode/frame-dsl-*.vsix"
expect "leaves the editors' own tests out" test ! -e "${here}/share/frame-dsl/editors/vim/frame-dsl-tests.vim" -a \
	! -e "${here}/share/frame-dsl/editors/emacs/frame-dsl-mode-tests.el"
expect "the editors' links to the manual resolve" \
	test -e "${here}/share/frame-dsl/editors/emacs/../../manual/editors.md"

# Each editor picks the support up from where the archive puts it, as INSTALL.md says to install it --
# each only when that editor is here, as `make test-editors` does.
printf 'pkt P {\n  id: 8 = 1;\n};\n' >"${tmp}/probe.fdsl"
if command -v emacs >/dev/null 2>&1; then
	expect "Emacs loads the mode from the archive" emacs --batch -Q \
		-l "${here}/share/frame-dsl/editors/emacs/frame-dsl-mode.el" \
		--eval "(progn (find-file \"${tmp}/probe.fdsl\") (kill-emacs (if (eq major-mode 'frame-dsl-mode) 0 1)))"
fi
if command -v vim >/dev/null 2>&1; then
	expect "Vim finds the filetype in the archive's runtime directory" vim -Nu NONE -i NONE -es \
		-c "set runtimepath+=${here}/share/frame-dsl/editors/vim" -c 'filetype plugin indent on' -c 'syntax on' \
		-c "edit ${tmp}/probe.fdsl" -c 'if &filetype !=# "fdsl" || !exists("b:current_syntax") | cquit | endif' -c 'qall!'
fi
vsix="$(compgen -G "${here}/share/frame-dsl/editors/vscode/frame-dsl-*.vsix" | head -1)"
expect "the VS Code extension is a whole archive" unzip -tq "$vsix"
# A release's extension is that release: a tag cut without bumping editors/vscode/package.json would
# otherwise ship last version's number inside this version's archive.
if [[ "$version" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	expect "its version is ${version#v}" test "$(basename "$vsix")" = "frame-dsl-${version#v}.vsix"
fi

expect "the tool says its version" "${here}/bin/frame-dsl" --version
if [ -n "$version" ]; then
	expect "and it is ${version}" test "$("${here}/bin/frame-dsl" --version)" = "frame-dsl ${version}"
fi
expect "the tool checks a definition" "${here}/bin/frame-dsl" "${root}/examples/optional_fields.fdsl" --check
expect "and refuses a wrong one" bash -c "! '${here}/bin/frame-dsl' '${root}/examples/invalid/if_reads_absent_field.fdsl' --check"

cat >"${tmp}/use.c" <<'C'
#include <stdio.h>
#include "framedsl.h"
int main(void) {
  FramedslResult *result = framedsl_load_string("pkt P { id: 8 = 1; v: 8; };", "p.fdsl");
  int ok = framedsl_ok(result);
  framedsl_result_free(result);
  return ok ? 0 : 1;
}
C
cc="${CC:-cc}"
expect "a program links the static library" "$cc" -std=c11 -I "${here}/include" "${tmp}/use.c" "${here}/lib/libframedsl.a" -o "${tmp}/static"
expect "and runs" "${tmp}/static"
expect "a program links the shared library" "$cc" -std=c11 -I "${here}/include" "${tmp}/use.c" -L "${here}/lib" -lframedsl -o "${tmp}/shared"
expect "and runs, finding it beside it" env LD_LIBRARY_PATH="${here}/lib" "${tmp}/shared"

if [ "$failures" -ne 0 ]; then
	echo "$failures check(s) failed on ${archive}."
	exit 1
fi
echo "The release archive holds what it should, and what it holds works."
