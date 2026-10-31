- **The tool sends results to stdout and problems to stderr.** A failed decode used to go to stdout with
  colour codes, even when piped, and a position in this project's source. A problem now goes to stderr
  as `frame-dsl: at byte 0: ...`, and a problem with a definition prints the same lines in every mode as
  `--check` does. On a terminal that name and `error:` are red and `warning:` is yellow; nothing is
  coloured in a pipe, with `NO_COLOR` set, or in a result, and describing a file prints plain text. Exit
  statuses do not change.
