#!/bin/bash
#
# Packages editors/vscode/ into a .vsix, with zip alone -- no vsce, no npm. `make vsix` is the
# usual way to run this; see editors/README.md for what goes in the archive and why there is no
# marketplace publishing.
#
# Usage: scripts/build_vsix.sh <output.vsix>
#
# A .vsix is a zip of three things: `[Content_Types].xml` and `extension.vsixmanifest`, which is
# what identifies it as one, and `extension/`, a copy of the extension itself -- read by
# `code --install-extension` off of `extension/package.json`, the same manifest VS Code reads once
# it is installed.
set -euo pipefail

out="${1:?usage: scripts/build_vsix.sh <output.vsix>}"
case "$out" in
/*) : ;;
*) out="$(pwd)/${out}" ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="${root}/editors/vscode"

log_halt() {
	printf "[HALT] - %s\n" "$*" >&2
	exit 1
}

command -v jq >/dev/null 2>&1 || log_halt "no jq on PATH -- it reads editors/vscode/package.json"
command -v zip >/dev/null 2>&1 || log_halt "no zip on PATH"
command -v unzip >/dev/null 2>&1 || log_halt "no unzip on PATH -- it checks the archive zip just wrote"
[ -f "${src}/vendor/vscode-languageclient.js" ] \
	|| log_halt "${src}/vendor/vscode-languageclient.js is missing -- run scripts/vendor_vscode_languageclient.sh"

name="$(jq -r .name "${src}/package.json")"
version="$(jq -r .version "${src}/package.json")"
publisher="$(jq -r .publisher "${src}/package.json")"
display_name="$(jq -r .displayName "${src}/package.json")"
description="$(jq -r .description "${src}/package.json")"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "${tmp}/extension"
cp "${src}/package.json" "${src}/language-configuration.json" "${src}/extension.js" "${tmp}/extension/"
cp "${root}/LICENSE" "${tmp}/extension/"
cp -r "${src}/syntaxes" "${tmp}/extension/syntaxes"
cp -r "${src}/vendor" "${tmp}/extension/vendor"

cat >"${tmp}/[Content_Types].xml" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="json" ContentType="application/json"/>
  <Default Extension="js" ContentType="application/javascript"/>
  <Default Extension="vsixmanifest" ContentType="text/xml"/>
</Types>
EOF

cat >"${tmp}/extension.vsixmanifest" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="${name}" Version="${version}" Publisher="${publisher}"/>
    <DisplayName>${display_name}</DisplayName>
    <Description xml:space="preserve">${description}</Description>
    <Categories>Programming Languages</Categories>
    <GalleryFlags>Public</GalleryFlags>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>
  </Assets>
</PackageManifest>
EOF

mkdir -p "$(dirname "$out")"
rm -f "$out"
(cd "$tmp" && zip -q -r "$out" '[Content_Types].xml' extension.vsixmanifest extension)

# What makes it a .vsix rather than any zip: the manifest, and the four files that are this
# extension. A missing one is a silent, un-installable archive otherwise. The license is not needed
# to install it, but MIT's one condition is that it stays with every copy.
listing="$(unzip -l "$out")"
for entry in 'extension.vsixmanifest' 'extension/package.json' 'extension/extension.js' \
	'extension/syntaxes/fdsl.tmLanguage.json' 'extension/vendor/vscode-languageclient.js' \
	'extension/LICENSE'; do
	echo "$listing" | grep -qF "$entry" || log_halt "${out} is missing ${entry}"
done

printf "[INFO] - %s\n" "built ${out}"
