"""The public API: loading, the three kinds of value, repetition, and one exception per status."""

import os
import tempfile
import unittest

import framedsl

EXAMPLES = os.path.join(os.path.dirname(__file__), "..", "..", "examples")


def example(name):
    return framedsl.load(os.path.join(EXAMPLES, name))


class Loading(unittest.TestCase):
    def test_an_invalid_definition_raises_with_every_diagnostic(self):
        source = "pkt A { id: 8 = 1; x: 3; };\npkt B { id: 8 = 2; n: 8; z[4]: valof(q) * 8; };\n"
        with self.assertRaises(framedsl.LoadError) as caught:
            framedsl.loads(source, name="two.fdsl")
        error = caught.exception
        self.assertEqual(error.status, 2)
        self.assertEqual(len(error.diagnostics), 2)
        for diagnostic in error.diagnostics:
            self.assertEqual(diagnostic.file, "two.fdsl")
            self.assertGreater(diagnostic.line, 0)
            self.assertGreater(diagnostic.column, 0)
            self.assertGreater(diagnostic.check, 0)
            self.assertEqual(diagnostic.severity, "error")
            self.assertIn(str(diagnostic), str(error))
        checks = {d.check for d in error.diagnostics}
        self.assertIn(13, checks)  # A: 11 bits, not a whole number of bytes.
        self.assertIn(7, checks)  # valof(q) names nothing.
        self.assertRegex(str(error.diagnostics[0]), r"^two\.fdsl:\d+:\d+: error: .* \[check \d+\]$")

    def test_a_diagnostic_in_an_import_names_its_own_file(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "broken.fdsl"), "w") as handle:
                handle.write("block B {\n  x: 3;\n  y: valof(q);\n};\n")
            main = os.path.join(directory, "main.fdsl")
            with open(main, "w") as handle:
                handle.write('import "broken.fdsl";\npkt P { id: 8 = 1; };\n')
            with self.assertRaises(framedsl.LoadError) as caught:
                framedsl.load(main)
        [diagnostic] = [d for d in caught.exception.diagnostics if "valof(q)" in d.message]
        self.assertEqual(os.path.basename(diagnostic.file), "broken.fdsl")
        self.assertIn("broken.fdsl:3:", str(caught.exception))

    def test_a_missing_file_is_a_load_error_with_the_io_status(self):
        with self.assertRaises(framedsl.LoadError) as caught:
            framedsl.load("/nonexistent/nothing.fdsl")
        self.assertEqual(caught.exception.status, 1)
        self.assertEqual(caught.exception.diagnostics[0].line, 0)
        self.assertIn("/nonexistent/nothing.fdsl", str(caught.exception))

    def test_warnings_on_a_definition_that_loads_are_kept(self):
        definition = framedsl.loads("block Spare { x: 8; };\npkt P { id: 8 = 1; };\n")
        self.assertEqual([d.severity for d in definition.diagnostics], ["warning"])

    def test_loads_with_a_path_resolves_imports_from_there(self):
        common = os.path.join(EXAMPLES, "imported_device.fdsl")
        with open(common) as handle:
            source = handle.read()
        definition = framedsl.loads(source, path=common)
        self.assertTrue(definition.packets)

    def test_introspection(self):
        definition = example("uart_device.fdsl")
        self.assertIn("TC_WRITE", definition.packets)
        self.assertEqual(definition.fields("TC_WRITE"), ["id", "size", "data", "crc"])
        with self.assertRaises(framedsl.FieldError):
            definition.fields("NOPE")


class Values(unittest.TestCase):
    def setUp(self):
        self.definition = example("uart_device.fdsl")
        self.frame = self.definition.frame()

    def test_the_sketch_in_the_issue(self):
        frame = self.frame
        frame["TC_WRITE.size"] = 2
        frame["TC_WRITE.data"] = b"\xaa\xbb"
        frame["TC_WRITE.crc"] = 0x1234
        wire = frame.encode("TC_WRITE")
        self.assertEqual(wire, bytes([0x03, 0x02, 0xAA, 0xBB, 0x12, 0x34]))

        receiver = self.definition.frame()
        self.assertEqual(receiver.decode(wire), "TC_WRITE")
        self.assertEqual(receiver["TC_WRITE.size"], 2)
        self.assertEqual(receiver["TC_WRITE.data"], b"\xaa\xbb")
        self.assertEqual(receiver["TC_WRITE.crc"], 0x1234)

    def test_a_scalar_takes_an_int_and_a_bit_string_takes_bytes(self):
        with self.assertRaises(TypeError):
            self.frame["TC_WRITE.size"] = b"\x02"
        with self.assertRaises(TypeError):
            self.frame["TC_WRITE.size"] = True
        with self.assertRaises(TypeError):
            self.frame["TC_WRITE.data"] = 2
        self.frame["TC_WRITE.data"] = bytearray(b"\x01")
        self.assertEqual(self.frame["TC_WRITE.data"], b"\x01")

    def test_each_accessor_error_is_its_own_exception(self):
        with self.assertRaises(framedsl.FieldError):
            self.frame["TC_WRITE.nope"]
        with self.assertRaises(framedsl.FieldError):
            self.frame.encode("NOPE")
        with self.assertRaises(framedsl.RangeError):
            self.frame["TC_WRITE.size"] = 256  # an 8-bit field
        with self.assertRaises(framedsl.RangeError):
            self.frame["TC_WRITE.size"] = -1
        with self.assertRaises(framedsl.RangeError):
            self.frame["TC_WRITE.data"] = bytes(257)  # 256 reserved
        with self.assertRaises(framedsl.UnrecognisedError):
            self.frame.decode(b"\xee\x00\x00")
        with self.assertRaises(framedsl.TruncatedError):
            self.frame.decode(b"\x03\x05\xaa")
        # Every one of them is a framedsl.Error, carrying the status the C call returned.
        statuses = {
            framedsl.FieldError: 4,
            framedsl.RangeError: 5,
            framedsl.BufferSizeError: 6,
            framedsl.UnrecognisedError: 7,
            framedsl.TruncatedError: 8,
        }
        for kind, status in statuses.items():
            self.assertTrue(issubclass(kind, framedsl.Error))
            self.assertEqual(kind.status, status)
        self.assertEqual(len(set(statuses)), len(statuses))

    def test_a_failed_decode_changes_nothing(self):
        self.frame["TC_WRITE.size"] = 7
        with self.assertRaises(framedsl.TruncatedError):
            self.frame.decode(b"\x03\x02\xaa")
        self.assertEqual(self.frame["TC_WRITE.size"], 7)

    def test_a_stream_is_walked_with_decode_prefix(self):
        sender = self.definition.frame()
        sender["TC_HK_REQ.sequence"] = 1
        stream = sender.encode("TC_HK_REQ")
        sender["TC_WRITE.size"] = 1
        sender["TC_WRITE.data"] = b"\x42"
        stream += sender.encode("TC_WRITE")

        names = []
        while stream:
            name, consumed = self.frame.decode_prefix(stream)
            names.append(name)
            stream = stream[consumed:]
        self.assertEqual(names, ["TC_HK_REQ", "TC_WRITE"])
        self.assertEqual(self.frame["TC_WRITE.data"], b"\x42")

    def test_reset(self):
        self.frame["TC_WRITE.size"] = 3
        self.frame.reset()
        self.assertEqual(self.frame["TC_WRITE.size"], 0)

    def test_a_frame_keeps_its_definition_alive(self):
        frame = example("uart_device.fdsl").frame()
        frame["TC_HK_REQ.sequence"] = 9
        self.assertEqual(frame.encode("TC_HK_REQ"), b"\x01\x00\x09")


class Engineering(unittest.TestCase):
    def setUp(self):
        self.definition = example("calibrated_sensor.fdsl")
        self.frame = self.definition.frame()

    def test_eng_reads_and_writes_through_the_curve(self):
        self.frame["TM_POWER.bus"] = 4095
        self.assertAlmostEqual(self.frame.eng["TM_POWER.bus"], 3.3)
        self.frame.eng["TM_POWER.bus"] = 1.65
        self.assertEqual(self.frame["TM_POWER.bus"], 2048)
        self.frame.eng["TM_POWER.sensor"] = 10000.0  # an explicit inverse
        self.assertAlmostEqual(self.frame.eng["TM_POWER.sensor"], 10000.0, delta=5)

    def test_raw_stays_raw(self):
        self.frame["TM_POWER.board"] = 800
        self.assertEqual(self.frame["TM_POWER.board"], 800)
        self.assertAlmostEqual(self.frame.eng["TM_POWER.board"], 10.0)

    def test_eng_on_a_field_without_a_curve_is_a_field_error(self):
        with self.assertRaises(framedsl.FieldError):
            self.frame.eng["TM_POWER.id"]

    def test_a_curve_without_an_inverse_cannot_be_set(self):
        definition = framedsl.loads("cal square(raw) = raw * raw;\npkt P { id: 8 = 1; [ @cal: square; ] x: 8; };\n")
        frame = definition.frame()
        frame["P.x"] = 3
        self.assertEqual(frame.eng["P.x"], 9.0)
        with self.assertRaises(framedsl.FieldError):
            frame.eng["P.x"] = 16.0

    def test_eng_out_of_range_is_a_range_error(self):
        with self.assertRaises(framedsl.RangeError):
            self.frame.eng["TM_POWER.bus"] = 100.0  # 12 bits cannot hold it

    def test_calibrations_by_name(self):
        self.assertIn("volts", self.definition.calibrations)
        self.assertAlmostEqual(self.definition.calibrate("volts", 4095), 3.3)
        self.assertAlmostEqual(self.definition.uncalibrate("volts", 3.3), 4095)
        self.assertIn("fdsl_volts", self.definition.emit_c())
        self.assertIn("my_volts", self.definition.emit_c("my_"))
        with self.assertRaises(framedsl.FieldError):
            self.definition.calibrate("nope", 1)


class Repetition(unittest.TestCase):
    def setUp(self):
        self.definition = example("sensor_samples.fdsl")
        self.frame = self.definition.frame()

    def test_elements_by_name_by_count_and_as_a_list(self):
        frame = self.frame
        frame["TM_SAMPLES.count"] = 3
        for i in range(3):
            frame[f"TM_SAMPLES.samples[{i}].channel"] = i
            frame[f"TM_SAMPLES.samples[{i}].reading"] = 0x100 * (i + 1)
        wire = frame.encode("TM_SAMPLES")
        self.assertEqual(
            wire.hex(" "),
            "30 00 00 03 a0 00 01 a1 00 02 a2 00 03",
        )

        receiver = self.definition.frame()
        self.assertEqual(receiver.decode(wire), "TM_SAMPLES")
        self.assertEqual(receiver.count("TM_SAMPLES.samples"), 3)
        self.assertEqual(receiver.count("TM_SAMPLES.samples.reading"), 3)
        self.assertEqual(receiver.elements("TM_SAMPLES.samples.reading"), [0x100, 0x200, 0x300])
        self.assertEqual(receiver.elements("TM_SAMPLES.samples.marker"), [0xA, 0xA, 0xA])
        self.assertEqual(receiver["TM_SAMPLES.samples[1].channel"], 1)
        # The whole repetition is a bit string.
        self.assertEqual(receiver["TM_SAMPLES.samples"], wire[4:])

    def test_a_calibrated_member(self):
        self.frame["TM_SAMPLES.count"] = 1
        self.frame.eng["TM_SAMPLES.samples[0].reading"] = 3.3
        self.assertEqual(self.frame["TM_SAMPLES.samples[0].reading"], 4095)

    def test_an_element_past_the_reservation_is_not_a_field(self):
        with self.assertRaises(framedsl.FieldError):
            self.frame["TM_SAMPLES.samples[16].reading"]

    def test_a_count_past_the_reservation_is_a_range_error(self):
        self.frame["TM_SAMPLES.count"] = 17
        with self.assertRaises(framedsl.RangeError):
            self.frame.count("TM_SAMPLES.samples")


if __name__ == "__main__":
    unittest.main()
