- **`frame-dsl <file> --list-packets`** and **`--list-fields <packet>`** print the packet names, or
  the fields `-e` takes, one per line -- and nothing at all, with exit status 1, when the file does
  not load or has no such packet. The completions use them; so can any script.
