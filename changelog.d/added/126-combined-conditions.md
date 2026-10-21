- **Conditions that combine comparisons.** An `@if` takes `&&`, `||` and parentheses, the orderings
  `<`, `<=`, `>` and `>=`, and a mask to test bits: `[ @if: valof(flags) & 0x4 != 0 && valof(version)
  >= 2; ]`. `&` binds tighter than the comparison, unlike C's, and `&&` tighter than `||`. A field typed
  by an enum is compared only with `==` and `!=` (check 43, new), and a mask with bits the field does
  not have, or an equality the mask alone decides, is check 8. Check 42 compares conditions by their
  `&&` terms as written. `examples/combined_conditions.fdsl` shows each.
