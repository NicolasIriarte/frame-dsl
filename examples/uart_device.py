#!/usr/bin/env python3
"""The same worked example as examples/uart_device.c, from Python: a UART driver talking to a device
that is not there.

    PYTHONPATH=python FRAMEDSL_LIBRARY=build/libframedsl.so.0 examples/uart_device.py

or, once `make python` has copied the library next to the package:

    PYTHONPATH=python examples/uart_device.py

Both files load examples/uart_device.fdsl and speak the same protocol over the same pretend link, so
this is worth reading beside uart_device.c rather than instead of it: what changes is how the frame is
addressed, not what a frame is. In C, every field name is resolved once, at startup, into a
FramedslFieldId; framedsl.Frame caches the same lookup itself, keyed by the dotted name, so a script
just writing frame["TM_HK.reading"] pays for it once too. TM_HK.reading also carries a calibration
(see uart_device.fdsl), so this version prints it through frame.eng[...] as well as the raw counts --
the one thing here that uart_device.c does not show.
"""

import sys

import framedsl


class Device:
    """Stands in for examples/uart_device.c's `static Device_t device`."""

    def __init__(self):
        self.sequence = 0
        self.active_channel = 0
        self.limit = 0


def uart_transmit(data):
    """Stands in for the peripheral."""
    print("  uart_transmit:", data.hex(" ").upper())


class Uart:
    """Resolves nothing up front -- framedsl.Frame caches each dotted name the first time it is used,
    which is the Python equivalent of examples/uart_device.c's `ids` struct filled in at uart_init().
    """

    def __init__(self, path=None):
        self.definition = framedsl.load(path or "examples/uart_device.fdsl")
        self.storage = self.definition.frame()
        self.device = Device()
        self._reading = 0  # whatever the ADC would say, in raw counts

    def receive(self, data):
        try:
            name, consumed = self.storage.decode_prefix(data)
        except framedsl.Error as error:
            # NULL in C, an exception here: bytes that are not a packet are an ordinary outcome on a
            # real link, not a bug to crash over.
            print(f"  uart: {len(data)} bytes that are not a packet ({error})")
            return False

        print(f"  received {name} ({consumed} of {len(data)} bytes)")

        if name == "TC_HK_REQ":
            self._handle_hk_req()
            self._send_hk()
        elif name == "TC_SET_LIMIT":
            self._handle_set_limit()
            self._send_ack(0)
        elif name == "TC_WRITE":
            self._handle_write()
            self._send_ack(1)
        else:
            # A packet this build has no handler for. Not acked: the sequence in `device` belongs to
            # some earlier command, and acking with it is worse than staying quiet.
            print(f"  uart: no handler for {name}")
            return False

        if consumed < len(data):
            print(f"  uart: {len(data) - consumed} bytes left over, another frame is waiting")
        return True

    def _handle_hk_req(self):
        self.device.sequence = self.storage["TC_HK_REQ.sequence"]

    def _handle_set_limit(self):
        frame = self.storage
        self.device.sequence = frame["TC_SET_LIMIT.sequence"]
        self.device.active_channel = frame["TC_SET_LIMIT.channel"]
        self.device.limit = frame["TC_SET_LIMIT.limit"]
        print(f"  channel {self.device.active_channel} limit set to {self.device.limit}")

    def _handle_write(self):
        frame = self.storage
        size = frame["TC_WRITE.size"]
        data = frame["TC_WRITE.data"]
        crc = frame["TC_WRITE.crc"]
        print(f"  size: {size}")
        print("  data:", data.hex(" ").upper(), f"({len(data)} bytes)")
        print(f"  crc: {crc:04X}")

    def _send_ack(self, code):
        frame = self.storage
        frame["TM_ACK.sequence"] = self.device.sequence
        frame["TM_ACK.result"] = code
        uart_transmit(frame.encode("TM_ACK"))

    def _send_hk(self):
        frame = self.storage
        self._reading += 17
        frame["TM_HK.sequence"] = self.device.sequence
        frame["TM_HK.channel"] = self.device.active_channel
        frame["TM_HK.reading"] = self._reading
        frame["TM_HK.limit"] = self.device.limit
        # frame.eng[...] runs TM_HK.reading's calibration on the way out -- the one thing this version
        # shows that uart_device.c does not.
        print(f"  reading: {self._reading} counts = {frame.eng['TM_HK.reading']:.3f} mV")
        uart_transmit(frame.encode("TM_HK"))


def main(argv):
    uart = Uart(argv[1] if len(argv) > 1 else None)

    # TC_SET_LIMIT: id 0x02, sequence 1, channel 3, limit 4200 (0x1068).
    SET_LIMIT = bytes([0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x10, 0x68])

    # TC_WRITE: id 0x03, size 4, data, crc.
    WRITE = bytes([0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB])

    # TC_HK_REQ: id 0x01, sequence 2.
    HK_REQ = bytes([0x01, 0x00, 0x02])
    NOISE = bytes([0xFF, 0xEE, 0xDD])

    print("TC_SET_LIMIT:")
    uart.receive(SET_LIMIT)

    print("TC_WRITE:")
    uart.receive(WRITE)

    print("TC_HK_REQ:")
    uart.receive(HK_REQ)

    print("noise:")
    uart.receive(NOISE)

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except framedsl.LoadError as error:
        print(error, file=sys.stderr)
        sys.exit(1)
