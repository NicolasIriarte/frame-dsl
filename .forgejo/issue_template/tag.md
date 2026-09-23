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
see `CHANGELOG.md`'s `[Unreleased]` section for what that is.

# Test

- [ ] `CHANGELOG.md` has something under `[Unreleased]` before this closes it out -- a tag with nothing
      to say fails the release job rather than publishing a page with no notes.
- [ ] CI is green on the commit being tagged.

# Tasks

- [ ] Rename `[Unreleased]` to `[X.Y.Z] - YYYY-MM-DD`, and open a fresh `[Unreleased]` above it.
- [ ] Merge the PR, tag the merge commit `vX.Y.Z`, and push the tag.
- [ ] `make vsix`, and attach the `.vsix` it writes to this tag's Forgejo release.
