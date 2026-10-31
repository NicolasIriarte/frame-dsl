- **`framedsl_present()` and `framedsl_count()` agree with a decoded frame.** When a fixed field past
  the ones that identify a packet differed from the definition, the decode followed the wire and these
  two answered from the definition: a field the frame did not carry was reported present, and read as a
  zero nobody sent, and a repetition reported elements it did not hold. A decoded frame now answers from
  what arrived. A frame being built still answers from the definition's value, and a conditional fixed
  field a decode did not carry reads its fixed value once its condition holds, where it read zero.
