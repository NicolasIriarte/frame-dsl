"""The public API: loading, the kinds of value, repetition, and one exception per status."""

import os
import tempfile
import unittest

import framedsl

EXAMPLES = os.path.join(os.path.dirname(__file__), "..", "..", "examples")


def example(name):
    return framedsl.load(os.path.join(EXAMPLES, name))


class Loading(unittest.TestCase):
    def test_an_invalid_definition_raises_with_every_diagnostic(self):
        source = "pkt A { id: 8 := 1; x: 3; };\npkt B { id: 8 := 2; n: 8; z[4]: valof(q) * 8; };\n"
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
                handle.write('import "broken.fdsl";\npkt P { id: 8 := 1; };\n')
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
        definition = framedsl.loads("block Spare { x: 8; };\npkt P { id: 8 := 1; };\n")
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
        definition = framedsl.loads("cal square(raw) = raw * raw;\npkt P { id: 8 := 1; [ @cal: square; ] x: 8; };\n")
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


class Names(unittest.TestCase):
    def setUp(self):
        self.definition = example("radio_status.fdsl")
        self.frame = self.definition.frame()

    def test_names_read_and_write_and_the_number_stays_the_number(self):
        frame = self.frame
        frame.names["TM_ANTENNA.power"] = "ON"
        frame.names["TM_ANTENNA.level"] = "HIGH"
        frame.names["TM_ANTENNA.mode"] = "TRANSMIT"
        self.assertEqual(frame["TM_ANTENNA.level"], 3)
        self.assertEqual(frame.encode("TM_ANTENNA").hex(" "), "10 00 b0 02 01")
        frame["TM_ANTENNA.level"] = 7
        self.assertEqual(frame.names["TM_ANTENNA.level"], "MAX")

    def test_an_unlisted_value_decodes_and_reads_as_a_number(self):
        frame = self.frame
        self.assertEqual(frame.decode(bytes.fromhex("10 00 d0 00 01")), "TM_ANTENNA")
        self.assertEqual(frame["TM_ANTENNA.level"], 5)
        with self.assertRaises(framedsl.UnlistedError):
            frame.names["TM_ANTENNA.level"]

    def test_an_unknown_name_is_refused_and_nothing_is_written(self):
        self.frame.names["TM_ANTENNA.level"] = "LOW"
        with self.assertRaises(framedsl.UnlistedError):
            self.frame.names["TM_ANTENNA.level"] = "LOUD"
        self.assertEqual(self.frame.names["TM_ANTENNA.level"], "LOW")
        with self.assertRaises(framedsl.FieldError):
            self.frame.names["TM_ANTENNA.spare"]
        with self.assertRaises(TypeError):
            self.frame.names["TM_ANTENNA.level"] = 3

    def test_every_enum_and_its_names(self):
        enums = self.definition.enums
        self.assertEqual(list(enums), ["PktType", "PowerState", "TxLevel", "Mode"])
        self.assertEqual(enums["TxLevel"], {"OFF": 0, "LOW": 1, "MID": 2, "HIGH": 3, "MAX": 7})
        self.assertEqual(enums["Mode"], {"IDLE": 0x100, "RECEIVE": 0x101, "TRANSMIT": 0x102})
        self.assertIn("fdsl_TxLevel_name", self.definition.emit_c())


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


class Conditions(unittest.TestCase):
    """Conditional fields (#12): present only when an earlier field says so."""

    def setUp(self):
        self.definition = example("optional_fields.fdsl")
        self.frame = self.definition.frame()

    def test_an_absent_field_raises_and_present_says_so(self):
        self.frame.decode(bytes([0x50, 0x00]))
        self.assertFalse(self.frame.present("TM_OPTIONAL.time.seconds"))
        self.assertTrue(self.frame.present("TM_OPTIONAL.hdr.kind"))
        with self.assertRaises(framedsl.AbsentError) as caught:
            self.frame["TM_OPTIONAL.time.seconds"]
        self.assertEqual(caught.exception.status, 10)
        self.assertIsInstance(caught.exception, LookupError)
        with self.assertRaises(framedsl.AbsentError):
            self.frame.count("TM_OPTIONAL.readings")
        with self.assertRaises(framedsl.AbsentError):
            self.frame.names["TM_OPTIONAL.code"]

    def test_a_packet_the_caller_names(self):
        definition = framedsl.loads(
            "@decode: manual;\n"
            "pkt ENGINE { rpm: 16; temp: 8; };\n"
            "pkt BATTERY { volts: 16; amps: 16; };\n"
            "[ @decode: auto; ]\npkt PING { id: 8 := 1; };\n"
        )
        frame = definition.frame()
        self.assertEqual(frame.decode(bytes.fromhex("01 02 03"), packet="ENGINE"), "ENGINE")
        self.assertEqual(frame["ENGINE.rpm"], 0x0102)
        self.assertEqual(frame.decode_prefix(bytes.fromhex("01 02 03 04 05"), packet="BATTERY"), ("BATTERY", 4))
        with self.assertRaises(framedsl.FieldError):
            frame.decode(b"\x01", packet="PING")
        with self.assertRaises(framedsl.TruncatedError):
            frame.decode(b"\x01", packet="ENGINE")
        with self.assertRaises(framedsl.UnrecognisedError):
            frame.decode(bytes.fromhex("02 02 03"))

    def test_a_field_reads_by_its_type(self):
        definition = framedsl.loads(
            "cal celsius(raw) = raw * 0.0625;\n"
            "pkt P { id: 8 := 0x30; [ @cal: celsius; ] temp: i16; gain: f32; };\n"
        )
        frame = definition.frame()
        frame["P.temp"] = -160
        frame["P.gain"] = 1.5
        self.assertEqual(frame.encode("P"), bytes.fromhex("30 FF60 3FC00000"))
        self.assertEqual(frame["P.temp"], -160)
        self.assertEqual(frame["P.gain"], 1.5)
        self.assertEqual(frame.eng["P.temp"], -10.0)
        self.assertEqual(frame.raw["P.temp"], 0xFF60)
        self.assertEqual(frame.raw["P.gain"], 0x3FC00000)

        frame.raw["P.gain"] = 0x7FC00001
        self.assertNotEqual(frame["P.gain"], frame["P.gain"])  # a NaN
        with self.assertRaises(framedsl.RangeError):
            frame["P.temp"] = 40000
        with self.assertRaises(TypeError):
            frame["P.temp"] = 1.5
        with self.assertRaises(TypeError):
            frame["P.gain"] = "1.5"

    def test_a_default_is_held_until_replaced(self):
        definition = framedsl.loads(
            "enum Health: 8 { FAULT; OK; };\n"
            "block Channel { state: Health ?= OK; };\n"
            "pkt P { id: 8 := 0x40; spare: 8 ?= 0; temp: Health ?= OK; channels: Channel[2]; };\n"
        )
        frame = definition.frame()
        self.assertEqual(frame.encode("P"), bytes.fromhex("40 00 01 01 01"))
        frame.names["P.temp"] = "FAULT"
        frame.names["P.channels[1].state"] = "FAULT"
        self.assertEqual(frame.encode("P"), bytes.fromhex("40 00 00 01 00"))
        frame.reset()
        self.assertEqual(frame.encode("P"), bytes.fromhex("40 00 01 01 01"))
        self.assertEqual(frame.decode(bytes.fromhex("40 80 00 01 01")), "P")
        self.assertEqual(frame["P.spare"], 0x80)

    def test_a_fixed_field_cannot_be_set(self):
        definition = framedsl.loads(
            "enum Mode: 8 { OFF; ON; };\n"
            "cal twice(raw) = raw * 2;\n"
            "block Item { mark: 4 := 0xA; value: 4; };\n"
            "pkt P { id: 8 := 0x10; [ @cal: twice; ] scale: 8 := 4; mode: Mode := ON; items: Item[2]; };\n"
        )
        frame = definition.frame()
        self.assertEqual(frame["P.id"], 0x10)
        self.assertEqual(frame["P.items[1].mark"], 0xA)
        for value in (0x10, 0x55):
            with self.assertRaises(framedsl.FixedError):
                frame["P.id"] = value
        with self.assertRaises(framedsl.FixedError):
            frame["P.items[1].mark"] = 3
        with self.assertRaises(framedsl.FixedError):
            frame.eng["P.scale"] = 20.0
        with self.assertRaises(framedsl.FixedError):
            frame.names["P.mode"] = "OFF"
        self.assertIsInstance(framedsl.FixedError("x"), framedsl.Error)
        self.assertEqual(frame.encode("P"), bytes([0x10, 0x04, 0x01, 0xA0, 0xA0]))

    def test_an_absent_field_can_be_set_and_is_left_out(self):
        self.frame["TM_OPTIONAL.hdr.has_crc"] = 1
        self.frame["TM_OPTIONAL.crc"] = 0x1234
        self.frame["TM_OPTIONAL.time.seconds"] = 7  # absent: kept, not sent
        self.assertEqual(self.frame.encode("TM_OPTIONAL"), bytes([0x50, 0x40, 0x12, 0x34]))
        self.frame["TM_OPTIONAL.hdr.sec_hdr"] = 1
        self.assertTrue(self.frame.present("TM_OPTIONAL.time.seconds"))
        self.assertEqual(self.frame["TM_OPTIONAL.time.seconds"], 7)
        self.assertEqual(
            self.frame.encode("TM_OPTIONAL"), bytes([0x50, 0xC0, 0, 0, 0, 7, 0, 0, 0x12, 0x34])
        )

    def test_a_nested_part_and_a_repetition_round_trip(self):
        data = bytes.fromhex("50C2000000010002010700 05ABCD1234".replace(" ", ""))
        self.assertEqual(self.frame.decode(data), "TM_OPTIONAL")
        self.assertEqual(self.frame.names["TM_OPTIONAL.hdr.kind"], "EVENT")
        self.assertEqual(self.frame.elements("TM_OPTIONAL.readings.value"), [5])
        self.assertEqual(self.frame["TM_OPTIONAL.code"], 0xABCD)
        self.assertEqual(self.frame.encode("TM_OPTIONAL"), data)
