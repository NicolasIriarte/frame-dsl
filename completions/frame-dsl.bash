# Bash completion for frame-dsl (#85).
#
# Installed by `make install` to $(PREFIX)/share/bash-completion/completions/frame-dsl, where
# bash-completion loads it the first time `frame-dsl <TAB>` is typed. To try it without installing:
#
#   source completions/frame-dsl.bash
#
# Nothing here is a copy of the tool's own knowledge. The flags come from `frame-dsl --help` at the
# moment <TAB> is pressed, so they cannot drift from the parser. The tool asked is whichever one is
# being completed -- `./build/frame-dsl` as much as the installed one. No flag takes a packet or a
# field name since encoding left the command line (#97), so nothing else needs asking.
# completions/_frame-dsl is the same for zsh.

# The option lines of --help, as "flag takes-argument" pairs: `-d 1`, `--decode 1`, `--check 0`.
_frame_dsl_flags() {
	"$1" --help 2>/dev/null | awk '
		/^options:/ { taking = 1; next }
		/^$/        { taking = 0 }
		taking {
			n = 0
			for (i = 1; i <= NF; i++) {
				word = $i
				sub(/,$/, "", word)
				if (word ~ /^-/) { flags[++n] = word; continue }
				takes = word ~ /^</
				break
			}
			for (j = 1; j <= n; j++) print flags[j], takes
		}
	'
}

_frame_dsl() {
	local program="${COMP_WORDS[0]}"
	local cur="${COMP_WORDS[COMP_CWORD]}"
	local prev=""
	((COMP_CWORD > 0)) && prev="${COMP_WORDS[COMP_CWORD - 1]}"
	program="${program/#\~/$HOME}"
	COMPREPLY=()

	local flag_table
	flag_table="$(_frame_dsl_flags "$program")"
	local -A takes_argument=()
	local flag takes
	while read -r flag takes; do
		[[ -n "$flag" ]] && takes_argument[$flag]="$takes"
	done <<<"$flag_table"

	if [[ "$prev" == --stdin-name ]]; then
		compopt -o filenames 2>/dev/null
		mapfile -t COMPREPLY < <(compgen -f -- "$cur")
		return 0
	fi
	# Any other flag that takes an argument (-d's hex) has nothing worth offering.
	if [[ "${takes_argument[${prev:-_}]:-0}" == 1 ]]; then
		return 0
	fi

	if [[ "$cur" == -* ]]; then
		mapfile -t COMPREPLY < <(compgen -W "$(cut -d' ' -f1 <<<"$flag_table")" -- "$cur")
		return 0
	fi

	# Otherwise a definition file: .fdsl files, and directories to find one in.
	compopt -o filenames 2>/dev/null
	mapfile -t COMPREPLY < <(compgen -f -X '!*.fdsl' -- "$cur"; compgen -d -- "$cur")
	return 0
}

complete -F _frame_dsl frame-dsl
