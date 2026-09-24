# Changelog fragments

One file per changelog entry. When a version is cut, `make changelog VERSION=vX.Y.Z` folds them into
`CHANGELOG.md` as that version's section and deletes the ones it used.

This directory exists so that no two pull requests edit the same lines. Every pull request used to add
its entry to the top of `### Added` under `## [Unreleased]`, so any two open at once collided -- #72's
and #73's both conflicted with `main` on `CHANGELOG.md` the moment #86's merged, and `nightly` needed a
hand-made commit to drop an entry a merge had left twice. Forgejo's merge button cannot resolve
that, so every merge to `main` meant re-merging and re-pushing every other open branch (#90).

`.gitattributes` sets `merge=union` on `CHANGELOG.md`, which helps a merge made in a working tree --
`nightly`'s, or a branch cut before this directory existed. It does nothing for Forgejo's own merge: git
applies a merge driver only when `.gitattributes` is a real file in the tree doing the merge, and
Forgejo merges without one.

## Writing one

Create `changelog.d/<section>/<issue>-<slug>.md`:

    changelog.d/added/73-quick-fixes.md

- **`<section>`** is one of `added`, `changed`, `deprecated`, `removed`, `fixed`, `security` -- the
  [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) headings, in lower case. Anything else is
  refused rather than guessed at.
- **`<issue>`** is the issue the change came from. It orders the entries within a section.
- **`<slug>`** is a few words in kebab-case. Nothing reads it; it makes two entries from one issue two
  different files, and lets a listing of this directory say what is coming.

The file holds the entry exactly as it will read in `CHANGELOG.md`, bullet and all:

```markdown
- **Quick fixes in the language server.** A diagnostic with one obvious repair carries it, and the
  editor offers it as a quick fix: ...
```

Assembly is concatenation: nothing re-wraps or re-indents what you wrote, so what is reviewed in the
pull request is byte for byte what the release notes say. Start with `- `, wrap at 100 columns like the
rest of the file, and indent continuation lines by two spaces. One change can add as many fragments as
it has things to say, in as many sections; give each its own file.

A pull request that changes `src/`, `include/` or `editors/` must add at least one -- CI's `changelog`
job asks, and editing `CHANGELOG.md` instead does not count. Everything else (the manual, examples,
tests, scripts, CI) is work on the project rather than on what it ships, and owes nothing.

## Checking your work

    make changelog-preview     # what the next release's section would read like
    make test-changelog        # the rules themselves, without opening a pull request to find out
