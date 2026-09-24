- **Encoding from the command line.** `frame-dsl <file> -e <packet> field=value ...` is gone, and with
  it `-r`/`--report` and `-q`/`--quiet`, which only served it. Each is now an unexpected argument, and
  so is a `field=value`. Decoding (`-d`), describing, `--check`, `--format`, `--emit-c` and
  `--list-packets` are unchanged. A frame to send is built through the library; Python bindings for
  scripting it are #98.
