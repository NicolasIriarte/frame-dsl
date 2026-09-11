# Examples

Definition files kept valid against [`manual/language.md`](../manual/language.md). They are written
to double as the parser's fixtures: the files here must load, and every file under
[`invalid/`](invalid) must be **rejected**, each for one stated reason.

The negative fixtures are the more useful half. A parser that accepts good input is half a parser;
the checks in [`docs/design.md`](../docs/design.md) are most of what makes a definition file
trustworthy, and each file below exists so one of them has something to fail on.

## Should load

| File | What it shows |
| --- | --- |
| [`tm_datagram.fdsl`](tm_datagram.fdsl) | `valof()` with explicit unit conversion and a `max` bound |
| [`can_frame.fdsl`](can_frame.fdsl) | a real CAN 2.0A frame; reserved bits used as the discriminator |
| [`packet_family.fdsl`](packet_family.fdsl) | several packets in one file, told apart by a shared type field |
| [`bit_order.fdsl`](bit_order.fdsl) | `bit_order` and `byte_order` at file, packet and field scope |

## Should be rejected

| File | Check |
| --- | --- |
| [`invalid/no_discriminator.fdsl`](invalid/no_discriminator.fdsl) | 11 -- packet has no fixed-value field |
| [`invalid/ambiguous_packets.fdsl`](invalid/ambiguous_packets.fdsl) | 12 -- two packets cannot be told apart |
| [`invalid/unbounded_valof.fdsl`](invalid/unbounded_valof.fdsl) | 5 -- `valof()` width without `max` |
| [`invalid/forward_valof.fdsl`](invalid/forward_valof.fdsl) | 7 -- `valof()` names a later field |
| [`invalid/value_too_wide.fdsl`](invalid/value_too_wide.fdsl) | 8 -- fixed value does not fit its width |
| [`invalid/byte_order_unaligned.fdsl`](invalid/byte_order_unaligned.fdsl) | 10 -- `byte_order` on a field with no bytes |

## Why the two sketch packets are in separate files

`tm_datagram.fdsl` pins bits 0..7; `can_frame.fdsl` pins bits 12..13. They constrain **disjoint**
bits, so a frame could satisfy both discriminators at once and the decoder would have no basis to
choose. Putting them in one file is therefore rejected by check 12.

This is worth understanding before writing a real definition: having a discriminator each is not
enough, they have to *contradict*. `packet_family.fdsl` shows the pattern that works -- one type
field, at one offset, with a different value per packet.
