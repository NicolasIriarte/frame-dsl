# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/spec/v2.0.0.html).

Cutting a release closes out `[Unreleased]` and opens a fresh one above it; a tag whose version has
nothing written under it fails the release rather than publishing an empty page. See
`templates/tag_issue.md`.

## [Unreleased]

### Added

- **The language**, and a compiler for it: lexer, parser, and sixteen load-time semantic checks. A
  definition file declares packets of bit-width fields, with fixed values that identify them, widths
  that depend on another field's value, and ordering attributes at file, packet, block and field
  scope. See [`manual/language.md`](manual/language.md).
- **The runtime**: an arena with a dedicated slot per field, a bytecode the encoder and decoder walk,
  and a C API that sets fields by name, encodes a named packet, and decodes a buffer into whichever
  packet its fixed bits identify -- allocating nothing after a frame is created. See
  [`manual/api.md`](manual/api.md).

- A build: a `Makefile` producing `build/frame-dsl`, and a test runner built on CLibs' user-story
  test suite.
- The CLibs `logger`, `bash-logger` and `testing` libraries, vendored as release tarballs under
  `third_party/clibs/` and unpacked into `build/` by the build. Building needs no network and no
  credentials. `scripts/vendor_clibs.sh` updates them, and is the only thing here that does.
- Continuous integration: every pull request and every push to `main` builds and runs the test suite.
- `README.md`, and a written scope in `docs/scope.md` stating what the project will not do as well
  as what it will.
- `CHANGELOG.md`.
