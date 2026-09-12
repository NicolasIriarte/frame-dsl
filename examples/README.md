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
| [`tm_datagram.fdsl`](tm_datagram.fdsl) | `valof()` with explicit unit conversion and a reserved slot |
| [`ccsds_space_packet.fdsl`](ccsds_space_packet.fdsl) | a real CCSDS primary header: six sub-byte fields totalling exactly 48 bits |
| [`packet_family.fdsl`](packet_family.fdsl) | several packets in one file, told apart by a shared type field |
| [`bit_order.fdsl`](bit_order.fdsl) | attributes at every scope: file, packet, field and `block` |

## Should be rejected

| File | Check |
| --- | --- |
| [`invalid/no_discriminator.fdsl`](invalid/no_discriminator.fdsl) | 11 -- packet has no fixed-value field |
| [`invalid/ambiguous_packets.fdsl`](invalid/ambiguous_packets.fdsl) | 12 -- two packets cannot be told apart |
| [`invalid/unbounded_valof.fdsl`](invalid/unbounded_valof.fdsl) | 5 -- `valof()` width with no reservation |
| [`invalid/forward_valof.fdsl`](invalid/forward_valof.fdsl) | 7 -- `valof()` names a later field |
| [`invalid/value_too_wide.fdsl`](invalid/value_too_wide.fdsl) | 8 -- fixed value does not fit its width |
| [`invalid/byte_order_unaligned.fdsl`](invalid/byte_order_unaligned.fdsl) | 10 -- `@byte_order` on a field with no bytes |
| [`invalid/odd_packet.fdsl`](invalid/odd_packet.fdsl) | 13 -- packet is not a whole number of bytes |
| [`invalid/odd_valof_width.fdsl`](invalid/odd_valof_width.fdsl) | 13 -- variable width not provably a byte multiple |
| [`invalid/reservation_on_fixed_width.fdsl`](invalid/reservation_on_fixed_width.fdsl) | 14 -- a reservation on a constant-width field |
| [`invalid/order_on_a_bit_string.fdsl`](invalid/order_on_a_bit_string.fdsl) | 15 -- an ordering attribute written on a bit string |
| [`invalid/valof_on_a_bit_string.fdsl`](invalid/valof_on_a_bit_string.fdsl) | 16 -- `valof()` naming a field that is not a scalar |

Each of these fails the one check it is named for and no other. That is worth keeping true: a fixture
that trips two checks stops proving which one is working.

## Why these packets are in separate files

`packet_family.fdsl`'s `TC_PING` pins bits 0..7 to `0x01`; `ccsds_space_packet.fdsl` pins bits 0..3
to `0000`. The first four bits of `0x01` are `0000` as well, so the two **agree everywhere they
overlap** -- a frame could satisfy both discriminators at once and the decoder would have no basis to
choose. Putting them in one file is therefore rejected by check 12.

This is worth understanding before writing a real definition: having a discriminator each is not
enough, they have to *contradict*. Two packets pinning **disjoint** bits fail for the same reason,
since bits neither of them constrains cannot separate them either. `packet_family.fdsl` shows the
pattern that works -- one type field, at one offset, with a different value per packet.
