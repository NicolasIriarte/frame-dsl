"""The ctypes mirror on its own: framedsl/_native.py against the header, without the public API.

A struct declared with a field missing or out of order does not fail to load -- it reads every field
after the mistake from the wrong offset. So these read a definition whose every field is known, and
check every member of the structs that came back.
"""

import ctypes
import os
import subprocess
import sys
import unittest

from framedsl import _native

EXAMPLES = os.path.join(os.path.dirname(__file__), "..", "..", "examples")


def load(source):
    result = _native.framedsl_load_string(source.encode(), b"test.fdsl")
    return result, _native.framedsl_definition(result)


class Structs(unittest.TestCase):
    def test_a_field_reads_back_every_member(self):
        result = _native.framedsl_load_file(os.path.join(EXAMPLES, "sensor_samples.fdsl").encode())
        self.assertTrue(_native.framedsl_ok(result))
        definition = _native.framedsl_definition(result)
        packet = ctypes.c_size_t()
        self.assertTrue(_native.framedsl_find_packet(definition, b"TM_SAMPLES", ctypes.byref(packet)))

        # hdr.id, hdr.sequence, count, samples.
        self.assertEqual(_native.framedsl_field_count(definition, packet.value), 4)
        samples = _native.Field()
        self.assertTrue(_native.framedsl_field(definition, packet.value, 3, ctypes.byref(samples)))
        self.assertEqual(samples.name, b"samples")
        self.assertTrue(samples.variable)
        self.assertEqual(samples.max_bits, 48 * 8)
        self.assertEqual(samples.reserved_bits, 48 * 8)
        self.assertTrue(samples.offset_known)
        self.assertEqual(samples.offset_bits, 32)
        self.assertFalse(samples.is_scalar)
        self.assertFalse(samples.has_fixed)
        self.assertIsNone(samples.cal)
        self.assertEqual(samples.element_bits, 24)
        self.assertEqual(samples.member_count, 3)

        marker, reading = _native.Field(), _native.Field()
        self.assertTrue(_native.framedsl_member(definition, packet.value, 3, 0, ctypes.byref(marker)))
        self.assertTrue(_native.framedsl_member(definition, packet.value, 3, 2, ctypes.byref(reading)))
        self.assertEqual(marker.name, b"marker")
        self.assertTrue(marker.has_fixed)
        self.assertEqual(marker.fixed, 0xA)
        self.assertEqual(marker.width_bits, 4)
        self.assertEqual(reading.name, b"reading")
        self.assertFalse(reading.variable)
        self.assertEqual(reading.width_bits, 16)
        self.assertEqual(reading.offset_bits, 8)
        self.assertTrue(reading.is_scalar)
        self.assertEqual(reading.bit_order, _native.BIT_ORDER_MSB_FIRST)
        self.assertEqual(reading.byte_order, _native.BYTE_ORDER_LITTLE_ENDIAN)
        self.assertEqual(reading.cal, b"volts")
        self.assertTrue(reading.cal_invertible)
        self.assertEqual(reading.element_bits, 0)
        self.assertEqual(reading.member_count, 0)
        self.assertIsNone(reading.enumeration)
        _native.framedsl_result_free(result)

    def test_a_field_typed_by_an_enum_names_it_last(self):
        # Added after the first members (#11), so a Field missing it or holding it out of order reads it wrong.
        result = _native.framedsl_load_file(os.path.join(EXAMPLES, "radio_status.fdsl").encode())
        self.assertTrue(_native.framedsl_ok(result))
        definition = _native.framedsl_definition(result)
        level = _native.Field()
        self.assertTrue(_native.framedsl_field(definition, 0, 3, ctypes.byref(level)))
        self.assertEqual(level.name, b"level")
        self.assertEqual(level.width_bits, 3)
        self.assertIsNone(level.cal)
        self.assertEqual(level.enumeration, b"TxLevel")

        frame = _native.framedsl_frame_new(definition)
        field = _native.FieldId()
        self.assertTrue(_native.framedsl_find(definition, b"TM_ANTENNA.level", ctypes.byref(field)))
        self.assertEqual(_native.framedsl_set_name(frame, field, b"MAX"), _native.OK)
        name = ctypes.c_char_p()
        self.assertEqual(_native.framedsl_get_name(frame, field, ctypes.byref(name)), _native.OK)
        self.assertEqual(name.value, b"MAX")
        self.assertEqual(_native.framedsl_set_name(frame, field, b"LOUD"), _native.ERROR_UNLISTED)
        _native.framedsl_frame_free(frame)
        _native.framedsl_result_free(result)

    def test_a_conditional_field_names_its_condition_last(self):
        # The member added last (#12): a Field missing it reads past the end of what C wrote.
        result = _native.framedsl_load_file(os.path.join(EXAMPLES, "optional_fields.fdsl").encode())
        self.assertTrue(_native.framedsl_ok(result))
        definition = _native.framedsl_definition(result)
        seconds = _native.Field()
        self.assertTrue(_native.framedsl_field(definition, 0, 5, ctypes.byref(seconds)))
        self.assertEqual(seconds.name, b"time.seconds")
        self.assertEqual(seconds.condition, b"valof(hdr.sec_hdr) == 1")
        kind = _native.Field()
        self.assertTrue(_native.framedsl_field(definition, 0, 4, ctypes.byref(kind)))
        self.assertIsNone(kind.condition)

        frame = _native.framedsl_frame_new(definition)
        field = _native.FieldId()
        self.assertTrue(_native.framedsl_find(definition, b"TM_OPTIONAL.time.seconds", ctypes.byref(field)))
        present = ctypes.c_bool(True)
        self.assertEqual(_native.framedsl_present(frame, field, ctypes.byref(present)), _native.OK)
        self.assertFalse(present.value)
        value = ctypes.c_uint64()
        self.assertEqual(_native.framedsl_get_u64(frame, field, ctypes.byref(value)), _native.ERROR_ABSENT)
        _native.framedsl_frame_free(frame)
        _native.framedsl_result_free(result)

    def test_a_diagnostic_reads_back_every_member(self):
        # A misspelt attribute: an error with a position, a span and a suggested fix. It is a syntax
        # error, so its check is 0; test_frame.py covers numbered checks through the public API.
        result, _ = load("@bit_order: msb_first;\n@byte_ordr: big_endian;\npkt P { id: 8 := 1; };\n")
        self.assertFalse(_native.framedsl_ok(result))
        self.assertEqual(_native.framedsl_status(result), _native.ERROR_INVALID)
        self.assertEqual(_native.framedsl_source_name(result), b"test.fdsl")
        self.assertGreaterEqual(_native.framedsl_diagnostic_count(result), 1)
        diagnostic = _native.framedsl_diagnostic(result, 0).contents
        self.assertEqual((diagnostic.line, diagnostic.column), (2, 2))
        self.assertEqual((diagnostic.end_line, diagnostic.end_column), (2, 11))
        self.assertEqual(diagnostic.check, 0)
        self.assertIn(b"byte_ordr", diagnostic.message)
        self.assertIsNone(diagnostic.file)
        self.assertEqual(diagnostic.severity, _native.SEVERITY_ERROR)
        self.assertEqual(diagnostic.fix_text, b"byte_order")
        self.assertEqual((diagnostic.fix_line, diagnostic.fix_column), (2, 2))
        self.assertEqual((diagnostic.fix_end_line, diagnostic.fix_end_column), (2, 11))
        self.assertIn(b"byte_order", diagnostic.fix_title)
        _native.framedsl_result_free(result)

    def test_a_field_id_round_trips_through_element(self):
        result = _native.framedsl_load_file(os.path.join(EXAMPLES, "sensor_samples.fdsl").encode())
        definition = _native.framedsl_definition(result)
        member, indexed, chosen = _native.FieldId(), _native.FieldId(), _native.FieldId()
        self.assertTrue(_native.framedsl_find(definition, b"TM_SAMPLES.samples.reading", ctypes.byref(member)))
        self.assertEqual(member.element, _native.NO_ELEMENT)
        self.assertEqual(member.member, 3)
        self.assertTrue(_native.framedsl_find(definition, b"TM_SAMPLES.samples[5].reading", ctypes.byref(indexed)))
        self.assertEqual(_native.framedsl_element(definition, member, 5, ctypes.byref(chosen)), _native.OK)
        self.assertEqual(
            (chosen.owner, chosen.packet, chosen.field, chosen.element, chosen.member),
            (indexed.owner, indexed.packet, indexed.field, indexed.element, indexed.member),
        )
        _native.framedsl_result_free(result)


class Calls(unittest.TestCase):
    def test_encode_into_reports_the_size_and_leaves_a_short_buffer_alone(self):
        result, definition = load("pkt P { id: 8 := 0xAA; value: 16; };\n")
        frame = _native.framedsl_frame_new(definition)
        field = _native.FieldId()
        self.assertTrue(_native.framedsl_find(definition, b"P.value", ctypes.byref(field)))
        self.assertEqual(_native.framedsl_set_u64(frame, field, 0x1234), _native.OK)

        short = ctypes.create_string_buffer(b"\x55\x55", 2)
        length = ctypes.c_size_t()
        status = _native.framedsl_encode_into(frame, b"P", short, 2, ctypes.byref(length))
        self.assertEqual(status, _native.ERROR_BUFFER)
        self.assertEqual(length.value, 3)
        self.assertEqual(short.raw, b"\x55\x55")

        room = ctypes.create_string_buffer(3)
        self.assertEqual(_native.framedsl_encode_into(frame, b"P", room, 3, ctypes.byref(length)), _native.OK)
        self.assertEqual(room.raw, b"\xaa\x12\x34")
        _native.framedsl_frame_free(frame)
        _native.framedsl_result_free(result)

    def test_format_returns_memory_freed_with_the_c_free(self):
        out = ctypes.c_void_p()
        self.assertEqual(_native.framedsl_format(b"pkt P{id:8:=1;};", ctypes.byref(out)), _native.OK)
        self.assertIn(b"pkt P {", ctypes.string_at(out))
        _native.free(out)


class Finding(unittest.TestCase):
    """How the package finds the library, run in a child interpreter so the environment is its own."""

    def run_python(self, environment):
        env = {key: value for key, value in os.environ.items() if key != "FRAMEDSL_LIBRARY"}
        env.update(environment)
        return subprocess.run(
            [sys.executable, "-c", "import framedsl._native"], env=env, capture_output=True, text=True
        )

    def test_a_named_library_that_does_not_load_is_an_error_not_a_fallback(self):
        child = self.run_python({"FRAMEDSL_LIBRARY": "/nonexistent/libframedsl.so.0"})
        self.assertNotEqual(child.returncode, 0)
        self.assertIn("FRAMEDSL_LIBRARY=/nonexistent/libframedsl.so.0", child.stderr)

    def test_the_error_names_every_place_it_looked(self):
        # An empty package directory copy is not arranged here; this checks the message's shape when
        # the loader finds nothing, which is only reachable with no copy and no installed library.
        package = os.path.join(os.path.dirname(_native.__file__))
        copies = [name for name in os.listdir(package) if name.startswith("libframedsl.so")]
        if copies:
            self.skipTest("a copy of the library sits next to the package (`make python` was run)")
        child = self.run_python({"LD_LIBRARY_PATH": ""})
        if child.returncode == 0:
            self.skipTest("a libframedsl is installed on the system loader's path")
        self.assertIn("FRAMEDSL_LIBRARY", child.stderr)
        self.assertIn("next to the package", child.stderr)
        self.assertIn("system loader", child.stderr)


if __name__ == "__main__":
    unittest.main()
