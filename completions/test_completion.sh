#!/bin/bash
#
# Checks the Bash and zsh completions (#85), with no terminal: `make test-completion`, part of `make
# test`.
#
# Bash: the completion function is called the way readline calls it, with COMP_WORDS and COMP_CWORD
# set, and COMPREPLY read back. zsh: the function runs with `words` and CURRENT set and compsys'
# compadd, _files and _call_program replaced by stand-ins that record what they were asked to offer --
# the completion system itself needs a terminal, and what is being tested is what this file asks it
# to show. The zsh half is skipped, and says so, when zsh is not installed.
#
# Usage: completions/test_completion.sh <frame-dsl binary>
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
program="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
failures=0

pass() { printf "  ok   %s\n" "$1"; }
fail() {
	printf "  FAIL %s\n" "$1"
	failures=$((failures + 1))
}

# A directory to complete in: one definition that loads, one that does not, and things that are not
# definitions at all.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp "$root/examples/uart_device.fdsl" "$work/device.fdsl"
printf 'pkt TM {\n  id: 8\n};\n' >"$work/broken.fdsl"
printf 'not a definition\n' >"$work/notes.txt"
mkdir "$work/sub"

# What the flags --help lists, sorted -- what `--<TAB>` must offer, exactly.
help_flags="$("$program" --help | awk '/^options:/{t=1;next} /^$/{t=0} t' | grep -oE '(^| )--?[a-z][a-z-]*' | tr -d ' ' | sort -u)"

# bash_complete <words...>: the candidates for the last word, sorted, one per line. The last word is
# the one being completed, and may be "".
bash_complete() {
	(
		cd "$work" || exit 1
		# shellcheck source=completions/frame-dsl.bash
		source "$root/completions/frame-dsl.bash"
		COMP_WORDS=("$program" "$@")
		COMP_CWORD=$#
		_frame_dsl 2>/dev/null
		printf '%s\n' "${COMPREPLY[@]}" | sed '/^$/d' | sort
	)
}

# same <name> <want> <got>
same() {
	if [[ "$2" == "$3" ]]; then
		pass "$1"
	else
		fail "$1"
		diff <(printf '%s\n' "$2") <(printf '%s\n' "$3") | sed 's/^/       /'
	fi
}

lines() { printf '%s\n' "$@"; }

echo "completions/frame-dsl.bash"

same "--<TAB> offers exactly the flags --help lists" "$help_flags" "$(bash_complete "-")"
same "a definition completes to .fdsl files and directories" "$(lines broken.fdsl device.fdsl sub)" \
	"$(bash_complete "")"
same "-d's hex is not taken for a file, and offers nothing" "" "$(bash_complete device.fdsl -d "")"
same "--stdin-name <TAB> offers any file" "$(lines broken.fdsl device.fdsl notes.txt sub)" \
	"$(bash_complete - --check --stdin-name "")"
same "--packet <TAB> offers the definition's packets" "$("$program" "$work/device.fdsl" --list-packets | sort)" \
	"$(bash_complete device.fdsl -d 00 --packet "")"

if ! command -v zsh >/dev/null 2>&1; then
	echo "test_completion: no zsh on PATH, skipping completions/_frame-dsl"
else
	echo "completions/_frame-dsl"

	# zsh_complete <words...>: what the function handed compadd or _files, sorted, one per line.
	# compadd's -S suffix is appended to each candidate, as zsh would insert it; _files is recorded as
	# `<files: its arguments>`. PREFIX is the word being completed, as compsys sets it.
	zsh_complete() {
		(cd "$work" && zsh -f -c '
			program=$1; shift
			compadd() {
				local suffix= array=
				while [[ $1 == -* ]]; do
					case $1 in
						(-S) suffix=$2; shift 2 ;;
						(-a) array=$2; shift 2 ;;
						(--) shift; break ;;
						(*) shift ;;
					esac
				done
				local -a out
				if [[ -n $array ]]; then out=(${(P)array}); else out=("$@"); fi
				out=(${(M)out:#${PREFIX}*})
				(( $#out )) && print -l -- ${^out}$suffix
			}
			_files() { print -r -- "<files: $*>" }
			_call_program() { shift; eval "$@" }
			words=($program "$@")
			CURRENT=$#words
			PREFIX=$words[CURRENT]
			source "$0" 2>/dev/null
		' "$root/completions/_frame-dsl" "$program" "$@") | sort
	}

	same "--<TAB> offers exactly the flags --help lists" "$help_flags" "$(zsh_complete "-")"
	same "a definition completes to .fdsl files" "<files: -g *.fdsl(-.)>" "$(zsh_complete "")"
	same "-d's hex offers nothing" "" "$(zsh_complete device.fdsl -d "")"
	same "--stdin-name <TAB> offers any file" "<files: >" "$(zsh_complete - --check --stdin-name "")"
	same "--packet <TAB> offers the definition's packets" "$("$program" "$work/device.fdsl" --list-packets | sort)" \
		"$(zsh_complete device.fdsl -d 00 --packet "")"
fi

if [[ "$failures" -ne 0 ]]; then
	printf "[FAIL] - %d completion check(s) failed\n" "$failures" >&2
	exit 1
fi
echo "The completions offer what the tool takes."
