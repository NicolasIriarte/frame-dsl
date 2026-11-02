- **Real serial and CAN definitions in `examples/`.** `ublox_nav_posllh.fdsl` is a u-blox position message
  on a serial link, and `j1939.fdsl` is J1939 EEC1 and Address Claimed on CAN, each with a note of the
  document it comes from. A test decodes a reference frame of each to its published values, and encodes
  those back to the same bytes.
