# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[semantic versioning](https://semver.org/spec/v2.0.0.html).

Cutting a release closes out `[Unreleased]` and opens a fresh one above it; a tag whose version has
nothing written under it fails the release rather than publishing an empty page. See
`templates/tag_issue.md`.

## [Unreleased]

### Added

- A build: a `Makefile` producing `build/frame-dsl`, and a test runner built on CLibs' user-story
  test suite.
- The CLibs `logger`, `bash-logger` and `testing` libraries, vendored as release tarballs under
  `third_party/clibs/` and unpacked into `build/` by the build. Building needs no network and no
  credentials. `scripts/vendor_clibs.sh` updates them, and is the only thing here that does.
- Continuous integration: every pull request and every push to `main` builds and runs the test suite.
- `README.md`, and a written scope in `docs/scope.md` stating what the project will not do as well
  as what it will.
- `CHANGELOG.md`.
