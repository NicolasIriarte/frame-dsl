# Corpus

Definition files for the table in [`../system/7_corpus.c`](../system/7_corpus.c). Each one pins one
combination of the language's features; each row of that table names a file, the values to set, and
the bytes those values must produce.

These are **fixtures, not documentation**. [`examples/`](../../examples) is documentation that
happens to be tested -- files somebody reads to learn the language, with comments explaining the
protocol they model. A file here exists so that one combination of features has something to fail on,
and it is allowed to be as artificial as that requires.

Adding a case costs a file here and a row there. That is the whole point of the arrangement: the
combinations worth testing -- a sub-byte field next to a little-endian one, a `valof()` field at
exactly its reservation in a packet that also carries a bit string -- are the ones nobody writes a
test function for.

Every file in this directory must be named by at least one row. `us7_tc02` checks that, because a
fixture nobody loads is a fixture that has quietly stopped being valid.
