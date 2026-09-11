#!/bin/bash
#
# Writes a compile_commands.json describing how every source file in this project is compiled, so a
# language server can index it.
#
# Usage: scripts/gen_compile_commands.sh <output>
#
# Driven entirely by the Makefile, which passes the flag sets in as environment variables:
#
#   CC                the compiler
#   SRC_FLAGS         for src/*.c
#   UNIT_FLAGS        for test/unit/*.c
#   SYSTEM_FLAGS      for test/system/*.c
#
# That indirection is the point. A generator that restated the flags would be wrong the first time
# somebody edited a compile rule, and wrong *silently* -- the symptom is a confused language server
# rather than a failed build. The Makefile defines each set once and uses it in both places, so there
# is no second copy to drift.
#
# Not produced with bear or compiledb: both work by intercepting a build, so a developer would need
# one installed before the project was browsable, and this project holds the line that a clone builds
# with nothing but a compiler, make and tar.
set -euo pipefail

out="${1:?usage: gen_compile_commands.sh <output>}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

: "${CC:=cc}"
: "${SRC_FLAGS:=}"
: "${UNIT_FLAGS:=}"
: "${SYSTEM_FLAGS:=}"

# JSON has no escape for "whatever the shell gave us", so escaping is done here rather than hoped
# for. The flags genuinely contain double quotes -- the system tests are compiled with
# -DFRAMEDSL_BIN='"build/frame-dsl"' -- which is exactly the case a naive generator gets wrong.
json_escape() {
	local text="$1"
	text="${text//\\/\\\\}"
	text="${text//\"/\\\"}"
	printf '%s' "$text"
}

entries=""

emit_group() {
	local flags="$1"
	shift

	local file
	for file in "$@"; do
		[[ -e "$file" ]] || continue

		# One argument per array element, so a language server never has to re-split a command
		# line and guess where the quoting went.
		local arguments=""
		local token
		for token in $CC $flags -c "$file"; do
			arguments+="${arguments:+, }\"$(json_escape "$token")\""
		done

		entries+="${entries:+,}
  {
    \"directory\": \"$(json_escape "$root")\",
    \"file\": \"$(json_escape "$root/$file")\",
    \"arguments\": [${arguments}]
  }"
	done
}

cd "$root"

emit_group "$SRC_FLAGS" src/*.c
emit_group "$UNIT_FLAGS" test/unit/*.c
emit_group "$SYSTEM_FLAGS" test/system/*.c

mkdir -p "$(dirname "$out")"
printf '[%s\n]\n' "$entries" >"$out"
