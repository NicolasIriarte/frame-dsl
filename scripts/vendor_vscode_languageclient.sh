#!/bin/bash
#
# Updates editors/vscode/vendor/vscode-languageclient.js, the one bundled file the VS Code
# extension requires instead of a node_modules tree.
#
# Usage: scripts/vendor_vscode_languageclient.sh [version]   # default: the version already vendored
#
# This is NOT part of building or testing the extension: extension.js requires the committed bundle
# directly, and neither `make vsix` nor CI ever run npm. Run this by hand, occasionally, to bump
# vscode-languageclient -- then commit the file it replaces, the same way scripts/vendor_clibs.sh
# vendors CLibs.
#
# Unlike CLibs, there is no prebuilt single-file release to download: vscode-languageclient ships as
# an unbundled package with its own dependencies (vscode-jsonrpc, vscode-languageserver-protocol,
# vscode-languageserver-types), meant to be require()'d from node_modules. Producing one file needs a
# bundler, so this script needs npm on PATH -- the only place in this repository that is true. A
# machine without npm can still build and test everything else.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
vendor_dir="${root}/editors/vscode/vendor"
out="${vendor_dir}/vscode-languageclient.js"

log_info() { printf "[INFO] - %s\n" "$*"; }
log_halt() {
	printf "[HALT] - %s\n" "$*" >&2
	exit 1
}

command -v npm >/dev/null 2>&1 || log_halt "no npm on PATH -- get one (e.g. a local Node.js tarball from nodejs.org) and re-run"

# Fall back to the version already vendored, read out of the NOTICE file this script writes, so a
# plain run refreshes rather than silently moving to some other default.
current="$(sed -n 's/^vscode-languageclient version: //p' "${vendor_dir}/NOTICE" 2>/dev/null || true)"
VERSION="${1:-${current}}"
VERSION="${VERSION:-9.0.1}"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

log_info "installing vscode-languageclient@${VERSION} and esbuild into a scratch directory"
(
	cd "$tmp"
	npm init -y >/dev/null
	npm install --no-audit --no-fund --silent "vscode-languageclient@${VERSION}" esbuild
)

cat >"${tmp}/entry.js" <<'EOF'
module.exports = require('vscode-languageclient/node');
EOF

log_info "bundling into one CommonJS file, 'vscode' left external -- the extension host provides it"
mkdir -p "$vendor_dir"
(
	cd "$tmp"
	./node_modules/.bin/esbuild entry.js --bundle --platform=node --format=cjs --external:vscode \
		--minify --outfile="$out"
)
node --check "$out" || log_halt "the bundle esbuild produced does not parse -- something upstream changed shape"

license="$(cat "${tmp}/node_modules/vscode-languageclient/License.txt" 2>/dev/null || true)"
cat >"${vendor_dir}/NOTICE" <<EOF
vscode-languageclient version: ${VERSION}

This file (vscode-languageclient.js) is vscode-languageclient and its dependencies
(vscode-jsonrpc, vscode-languageserver-protocol, vscode-languageserver-types), bundled into one
CommonJS module by scripts/vendor_vscode_languageclient.sh. All are MIT-licensed, by Microsoft
Corporation. https://github.com/Microsoft/vscode-languageserver-node

${license}
EOF

log_info "vendored vscode-languageclient ${VERSION} into ${out}"
log_info "commit the vendor/ directory, and note the bump in CHANGELOG.md"
