"""The corpus table from test/system/7_corpus.c, run from Python.

Each row names a definition in test/corpus/, the values to set, and the bytes those values must
produce -- the same rows, values and bytes as the C table, so Python and C are checked against one
specification rather than against each other. As there, each row is encoded, compared byte for byte,
decoded into a second frame, and read back field by field; the second frame is what makes the read-back
mean anything.

A row added to the C table belongs here too. test_every_file_is_named keeps the two directories honest
the way us7_tc02 does: a corpus file neither table loads fails here.
"""

import os
import unittest

import framedsl

CORPUS = os.path.join(os.path.dirname(__file__), "..", "..", "test", "corpus")


def scalar(dotted, value, reads_back=None):
    return (dotted, value, value if reads_back is None else reads_back)


def raw(dotted, data):
    return (dotted, data, data)


ROWS = [
    (
        "whole-byte fields in declaration order",
        "byte_aligned.fdsl",
        "BYTE_ALIGNED",
        [scalar("BYTE_ALIGNED.seq", 0x1234), scalar("BYTE_ALIGNED.tail", 0x5F)],
        bytes([0xAA, 0x12, 0x34, 0x5F]),
    ),
    (
        "sub-byte fields packing and straddling",
        "sub_byte.fdsl",
        "SUB_BYTE",
        [
            scalar("SUB_BYTE.sec", 1),
            scalar("SUB_BYTE.apid", 0x555),
            scalar("SUB_BYTE.seq_flags", 3),
            scalar("SUB_BYTE.seq_count", 1),
        ],
        bytes([0x0D, 0x55, 0xC0, 0x01]),
    ),
    (
        "lsb_first beside msb_first",
        "bit_order.fdsl",
        "BIT_ORDER",
        [scalar("BIT_ORDER.a", 0x0F), scalar("BIT_ORDER.b", 0x0F)],
        bytes([0xAA, 0x0F, 0xF0]),
    ),
    (
        "little_endian swapping only its own field",
        "byte_order.fdsl",
        "BYTE_ORDER",
        [scalar("BYTE_ORDER.big", 0x11223344), scalar("BYTE_ORDER.little", 0x11223344)],
        bytes([0xBB, 0x11, 0x22, 0x33, 0x44, 0x44, 0x33, 0x22, 0x11, 0xBB]),
    ),
    (
        "a discriminator the caller wrote over",
        "fixed_over.fdsl",
        "FIXED_OVER",
        [scalar("FIXED_OVER.hdr", 0x55, reads_back=0xAA), scalar("FIXED_OVER.body", 0x01)],
        bytes([0xAA, 0x01]),
    ),
    (
        "a bit string wider than a scalar",
        "bit_string.fdsl",
        "BIT_STRING",
        [raw("BIT_STRING.wide", bytes([1, 2, 3, 4, 5, 6, 7, 8, 9]))],
        bytes([0xAA, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
    ),
    (
        "a bit string off a byte boundary",
        "bit_string_offset.fdsl",
        "BIT_STRING_OFFSET",
        [
            raw("BIT_STRING_OFFSET.wide", bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x11])),
            scalar("BIT_STRING_OFFSET.pad", 0x5),
        ],
        bytes([0xA1, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x15]),
    ),
    (
        "a valof() field at zero",
        "variable.fdsl",
        "VARIABLE",
        [scalar("VARIABLE.len", 0), scalar("VARIABLE.crc", 0xC7)],
        bytes([0xAA, 0x00, 0xC7]),
    ),
    (
        "a valof() field at a middling length",
        "variable.fdsl",
        "VARIABLE",
        [scalar("VARIABLE.len", 3), raw("VARIABLE.data", bytes([1, 2, 3])), scalar("VARIABLE.crc", 0xC7)],
        bytes([0xAA, 0x03, 1, 2, 3, 0xC7]),
    ),
    (
        "a valof() field at exactly its reservation",
        "variable.fdsl",
        "VARIABLE",
        [
            scalar("VARIABLE.len", 8),
            raw("VARIABLE.data", bytes([1, 2, 3, 4, 5, 6, 7, 8])),
            scalar("VARIABLE.crc", 0xC7),
        ],
        bytes([0xAA, 0x08, 1, 2, 3, 4, 5, 6, 7, 8, 0xC7]),
    ),
    (
        "one of three packets, the shortest",
        "family.fdsl",
        "TC_PING",
        [scalar("TC_PING.seq", 0x0009)],
        bytes([0x01, 0x00, 0x09]),
    ),
    (
        "one of three packets, a fixed-width one",
        "family.fdsl",
        "TC_SET_MODE",
        [scalar("TC_SET_MODE.seq", 0x1234), scalar("TC_SET_MODE.mode", 0x7F)],
        bytes([0x02, 0x12, 0x34, 0x7F]),
    ),
    (
        "one of three packets, the variable one",
        "family.fdsl",
        "TC_PAYLOAD",
        [scalar("TC_PAYLOAD.len", 2), raw("TC_PAYLOAD.body", bytes([0xAA, 0xBB])), scalar("TC_PAYLOAD.crc", 0xC7)],
        bytes([0x03, 0x02, 0xAA, 0xBB, 0xC7]),
    ),
]


class Corpus(unittest.TestCase):
    def test_every_row_round_trips(self):
        for what, file, packet, fields, expected in ROWS:
            with self.subTest(row=what, file=file):
                definition = framedsl.load(os.path.join(CORPUS, file))

                sender = definition.frame()
                for dotted, value, _ in fields:
                    sender[dotted] = value
                wire = sender.encode(packet)
                self.assertEqual(wire.hex(" "), expected.hex(" "))

                receiver = definition.frame()
                self.assertEqual(receiver.decode_prefix(wire), (packet, len(expected)))
                for dotted, _, reads_back in fields:
                    self.assertEqual(receiver[dotted], reads_back, dotted)

    def test_every_file_is_named(self):
        named = {file for _, file, _, _, _ in ROWS}
        present = {name for name in os.listdir(CORPUS) if name.endswith(".fdsl")}
        self.assertEqual(present - named, set(), "corpus files no row loads")


if __name__ == "__main__":
    unittest.main()
