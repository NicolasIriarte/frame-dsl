- **The error for a misplaced file default states the rule it breaks.** A bare `@name: value;` written
  after a `cal` or a `block` used to be told it "may only appear before the first packet" -- which it
  was. It now says the defaults go before the first declaration of any kind, and names that
  declaration and the line it starts on. One written straight after a `[ ... ]` group is told to go
  inside the group instead. The manual and the grammar state the rule the same way.
