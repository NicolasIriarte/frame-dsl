# Examples

Definition files kept valid against [`manual/language.md`](../manual/language.md). They are written
to double as the parser's fixtures: the files here must load, and every file under
[`invalid/`](invalid) must be **rejected**, each for one stated reason.

[`uart_device.c`](uart_device.c) is different: it is the worked example of using the library, not a
parser fixture. A UART driver talking to a device that is not there -- load the definition once,
resolve every field name at startup, decode whatever arrives in the RX callback, answer it. Written
against [`include/framedsl.h`](../include/framedsl.h) alone and linked against the library the same
way your own program would be.

```sh
make example && ./build/uart_device
```

To take it apart, copy it out and build against the library:

```sh
make lib
cc -std=c11 -Wall -Wextra -Iinclude my_device.c -Lbuild -l:libframedsl.a -o my_device
```

`-l:libframedsl.a` links the static archive. `build/` holds the shared `libframedsl.so` too, and a plain
`-lframedsl` picks that one, which then needs `LD_LIBRARY_PATH=build` to run.

Then change a width in [`uart_device.fdsl`](uart_device.fdsl) and re-run **without recompiling**,
which is the whole point of the project in one step. It runs as part of `make test-system` (see
[`test/system/8_examples.c`](../test/system/8_examples.c)) so it cannot go stale silently -- it is an
example, and the test is only there to keep it true.

[`uart_device.py`](uart_device.py) is the same driver again, from [Python](../README.md#from-python):
same definition, same three commands answered, same bytes on the wire, so the two are worth reading
side by side -- what changes is how a field is addressed (a dotted string, cached by
`framedsl.Frame` itself, rather than a `FramedslFieldId` resolved up front), not what a frame is.

```sh
make python
PYTHONPATH=python examples/uart_device.py
```

That is the first of the two ways this project is used -- the library, linked into the software being
written. The other is `build/frame-dsl` against a capture, with no program in the middle; both are
described in [the README](../README.md#the-two-ways-this-gets-used).

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
| [`uart_device.fdsl`](uart_device.fdsl) | the TM/TC pair the worked example speaks, ids and all, plus a calibrated field |
| [`radio_status.fdsl`](radio_status.fdsl) | enums: fields typed by them, a discriminator fixed by name, and a little-endian one |
| [`default_values.fdsl`](default_values.fdsl) | defaults: `?=` beside `:=`, on a repetition, and replaced by a use |
| [`signed_and_float.fdsl`](signed_and_float.fdsl) | typed fields: signed and float values, with a curve, a condition and a default |
| [`can_bus.fdsl`](can_bus.fdsl) | `@decode: manual`: packets a program names, for a link that carries identity elsewhere |
| [`little_endian_bitfields.fdsl`](little_endian_bitfields.fdsl) | a J1939 NAME: fields inside a little-endian word, crossing bytes |
| [`optional_fields.fdsl`](optional_fields.fdsl) | conditional fields: `@if` on a block's use, an anonymous block and a field, nested, by enum name and through `valof(hdr.flag)` |
| [`combined_conditions.fdsl`](combined_conditions.fdsl) | conditions combined with `&&` and `\|\|`, orderings, and masks testing bits of a flags field |

## Should be rejected

| File | Check |
| --- | --- |
| [`invalid/no_discriminator.fdsl`](invalid/no_discriminator.fdsl) | 11 -- packet has no fixed-value field |
| [`invalid/auto_without_a_discriminator.fdsl`](invalid/auto_without_a_discriminator.fdsl) | 11 -- an `auto` packet in a `manual` file still needs a fixed field |
| [`invalid/ambiguous_packets.fdsl`](invalid/ambiguous_packets.fdsl) | 12 -- two packets cannot be told apart |
| [`invalid/unbounded_valof.fdsl`](invalid/unbounded_valof.fdsl) | 5 -- `valof()` width with no reservation |
| [`invalid/forward_valof.fdsl`](invalid/forward_valof.fdsl) | 7 -- `valof()` names a later field |
| [`invalid/value_too_wide.fdsl`](invalid/value_too_wide.fdsl) | 8 -- fixed value does not fit its width |
| [`invalid/signed_value_out_of_range.fdsl`](invalid/signed_value_out_of_range.fdsl) | 8 -- `0xFF` on an `i8` is 255, not -1 |
| [`invalid/block_named_like_a_type.fdsl`](invalid/block_named_like_a_type.fdsl) | 1 -- a block of a type's shape could never be used |
| [`invalid/valof_of_a_float.fdsl`](invalid/valof_of_a_float.fdsl) | 16 -- a float has no whole value to size a field with |
| [`invalid/type_width_out_of_range.fdsl`](invalid/type_width_out_of_range.fdsl) | 44 -- no `f16` |
| [`invalid/default_too_wide.fdsl`](invalid/default_too_wide.fdsl) | 8 -- a default does not fit its width |
| [`invalid/default_on_a_bit_string.fdsl`](invalid/default_on_a_bit_string.fdsl) | 8 -- a default needs a scalar |
| [`invalid/default_does_not_identify.fdsl`](invalid/default_does_not_identify.fdsl) | 11 -- a default identifies nothing |
| [`invalid/byte_order_unaligned.fdsl`](invalid/byte_order_unaligned.fdsl) | 10 -- two fields sharing a byte disagree on `@byte_order` |
| [`invalid/odd_packet.fdsl`](invalid/odd_packet.fdsl) | 13 -- packet is not a whole number of bytes |
| [`invalid/odd_valof_width.fdsl`](invalid/odd_valof_width.fdsl) | 13 -- variable width not provably a byte multiple |
| [`invalid/reservation_on_fixed_width.fdsl`](invalid/reservation_on_fixed_width.fdsl) | 14 -- a reservation on a constant-width field |
| [`invalid/order_on_a_bit_string.fdsl`](invalid/order_on_a_bit_string.fdsl) | 15 -- an ordering attribute written on a bit string |
| [`invalid/valof_on_a_bit_string.fdsl`](invalid/valof_on_a_bit_string.fdsl) | 16 -- `valof()` naming a field that is not a scalar |
| [`invalid/enum_width_out_of_range.fdsl`](invalid/enum_width_out_of_range.fdsl) | 35 -- an enum wider than 64 bits |
| [`invalid/enum_duplicate_name.fdsl`](invalid/enum_duplicate_name.fdsl) | 36 -- one name twice in an enum |
| [`invalid/enum_values_out_of_order.fdsl`](invalid/enum_values_out_of_order.fdsl) | 37 -- an enum's values not increasing |
| [`invalid/enum_value_too_wide.fdsl`](invalid/enum_value_too_wide.fdsl) | 38 -- an enum's value that does not fit its width |
| [`invalid/enum_unknown_name.fdsl`](invalid/enum_unknown_name.fdsl) | 39 -- a value that is not one of the field's enum's names |
| [`invalid/enum_with_cal.fdsl`](invalid/enum_with_cal.fdsl) | 40 -- a calibration on a field typed by an enum |
| [`invalid/enum_pasted.fdsl`](invalid/enum_pasted.fdsl) | 41 -- an enum written without a field name |
| [`invalid/if_reads_absent_field.fdsl`](invalid/if_reads_absent_field.fdsl) | 42 -- a `valof()` reading a field the frame may not carry |
| [`invalid/if_part_not_whole_bytes.fdsl`](invalid/if_part_not_whole_bytes.fdsl) | 13 -- a conditional part that is not whole bytes |
| [`invalid/if_number_on_enum.fdsl`](invalid/if_number_on_enum.fdsl) | 39 -- a condition on an enum field compared with a number |
| [`invalid/if_value_too_wide.fdsl`](invalid/if_value_too_wide.fdsl) | 8 -- a condition comparing a field with a value it cannot hold |
| [`invalid/if_mask_too_wide.fdsl`](invalid/if_mask_too_wide.fdsl) | 8 -- a mask with bits the field does not have |
| [`invalid/if_mask_clears_value.fdsl`](invalid/if_mask_clears_value.fdsl) | 8 -- an equality the mask alone decides |
| [`invalid/if_ordering_on_enum.fdsl`](invalid/if_ordering_on_enum.fdsl) | 43 -- an ordering on a field typed by an enum |
| [`invalid/discriminator_after_condition.fdsl`](invalid/discriminator_after_condition.fdsl) | 11 -- the only fixed value after a conditional field |
| [`invalid/if_in_repeated_block.fdsl`](invalid/if_in_repeated_block.fdsl) | 32 -- a conditional field inside a repeated block |
| [`invalid/if_reads_later_field.fdsl`](invalid/if_reads_later_field.fdsl) | 7 -- a condition naming a later field |

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
