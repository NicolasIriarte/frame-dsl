---
name: "Release tag"
about: "Cuts a version and tags it."
title: "Prepare for vX.Y.Z"
labels:
  - state/created
  - type/improvements
---

# Description

Cuts vX.Y.Z. The scope is whatever is merged to `main` and unreleased at the point this is filed --
`make changelog-preview VERSION=vX.Y.Z` prints what that is, from `changelog.d/` and whatever is still
under `[Unreleased]`.

# Test

- [ ] `make changelog-preview VERSION=vX.Y.Z` prints something before this closes it out -- a tag with
      nothing to say fails the release job rather than publishing a page with no notes.
- [ ] CI is green on the commit being tagged.
- [ ] The Forgejo release for vX.Y.Z has this version's section as its notes and the `.vsix` attached.

# Tasks

- [ ] `make changelog VERSION=vX.Y.Z`: folds `changelog.d/` into `CHANGELOG.md` as `[X.Y.Z] - YYYY-MM-DD`,
      opens a fresh `[Unreleased]` above it, and removes the fragments it used. Commit the result,
      deletions included.
- [ ] Read the assembled section as somebody deciding whether to install it would, and write the
      paragraph that opens it -- the assembler puts entries in order but cannot say what the release
      is *about*.
- [ ] Merge the PR, tag the merge commit `vX.Y.Z`, and push the tag. CI's `release` job publishes the
      release once `test` passes on it.
