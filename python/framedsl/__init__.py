"""Frame-DSL from Python: load a `.fdsl`, set fields by name, encode a frame, decode one back.

    import framedsl

    definition = framedsl.load("examples/uart_device.fdsl")
    frame = definition.frame()
    frame["TC_WRITE.size"] = 2
    frame["TC_WRITE.data"] = b"\\xAA\\xBB"
    wire = frame.encode("TC_WRITE")

    name = frame.decode(wire)          # "TC_WRITE"
    frame["TC_WRITE.size"]             # 2

Everything runs in the C library, through ctypes (framedsl/_native.py), so a frame built here is the
frame a C program builds: there is no second parser or runtime to keep agreeing with the first.

A field's value is an int when the field is a scalar (64 bits or fewer) and bytes otherwise -- which
one is decided by the field, as it is in C. `frame.eng[...]` reads and writes a calibrated field in
engineering units, and `frame.names[...]` a field typed by an enum by its names. Every error the C API
reports becomes an exception of its own, all of them under `framedsl.Error`.
"""

import ctypes
import os

from . import _native

__all__ = [
    "load",
    "loads",
    "Definition",
    "Frame",
    "Diagnostic",
    "Error",
    "LoadError",
    "FieldError",
    "RangeError",
    "BufferSizeError",
    "UnrecognisedError",
    "TruncatedError",
    "UnlistedError",
]

# ---- Exceptions -------------------------------------------------------------------------------


class Error(Exception):
    """Anything the library refused. `status` is the FramedslStatus it returned."""

    status = None

    def __init__(self, message, status=None):
        super().__init__(message)
        if status is not None:
            self.status = status


class LoadError(Error):
    """A definition that did not load: unreadable (FRAMEDSL_ERROR_IO) or wrong (FRAMEDSL_ERROR_INVALID).

    `diagnostics` holds every problem found, warnings included, in the order the library reports them;
    the message is the same list, one per line, in the shape `frame-dsl --check` prints.
    """

    def __init__(self, message, status, diagnostics):
        super().__init__(message, status)
        self.diagnostics = diagnostics


class FieldError(Error, LookupError):
    """FRAMEDSL_ERROR_FIELD: no such field or packet, or the wrong kind for what was asked of it."""

    status = _native.ERROR_FIELD


class RangeError(Error, ValueError):
    """FRAMEDSL_ERROR_RANGE: the value does not fit the field, or a length is past what it reserved."""

    status = _native.ERROR_RANGE


class BufferSizeError(Error):
    """FRAMEDSL_ERROR_BUFFER: a buffer was too small. `needed` is the size the library asked for."""

    status = _native.ERROR_BUFFER

    def __init__(self, message, needed=None):
        super().__init__(message)
        self.needed = needed


class UnrecognisedError(Error, ValueError):
    """FRAMEDSL_ERROR_UNRECOGNISED: nothing in the definition matches the bytes."""

    status = _native.ERROR_UNRECOGNISED


class TruncatedError(Error, ValueError):
    """FRAMEDSL_ERROR_TRUNCATED: a packet matched, and the bytes ran out before it did."""

    status = _native.ERROR_TRUNCATED


class UnlistedError(Error, ValueError):
    """FRAMEDSL_ERROR_UNLISTED: a value, or a name, that the field's enum does not list.

    Reading a field whose value is unlisted raises this from frame.names[...] only; frame[...] still
    reads the number, which is what arrived.
    """

    status = _native.ERROR_UNLISTED


_BY_STATUS = {
    _native.ERROR_FIELD: FieldError,
    _native.ERROR_RANGE: RangeError,
    _native.ERROR_BUFFER: BufferSizeError,
    _native.ERROR_UNRECOGNISED: UnrecognisedError,
    _native.ERROR_TRUNCATED: TruncatedError,
    _native.ERROR_UNLISTED: UnlistedError,
}


def _raise(status, what):
    """Raise the exception `status` stands for. FRAMEDSL_ERROR_MEMORY is Python's own MemoryError."""
    if status == _native.ERROR_MEMORY:
        raise MemoryError(what)
    kind = _BY_STATUS.get(status)
    if kind is None:
        raise Error(f"{what}: status {status}", status)
    raise kind(what)


def _check(status, what):
    if status != _native.OK:
        _raise(status, what)


def _text(value):
    return None if value is None else value.decode("utf-8", "replace")


# ---- Diagnostics ------------------------------------------------------------------------------


class Diagnostic:
    """One problem with a definition. Lines and columns count from 1; line 0 means no position.

    `check` is the numbered check from the design document, or 0 for a syntax error. `file` is the
    file the problem is in -- the loaded one, or an import. `fix` is (title, text, line, column,
    end_line, end_column) when there is one obvious repair, else None.
    """

    __slots__ = ("file", "line", "column", "end_line", "end_column", "check", "severity", "message", "fix")

    def __init__(self, native, source_name):
        self.file = _text(native.file) or source_name
        self.line = native.line
        self.column = native.column
        self.end_line = native.end_line
        self.end_column = native.end_column
        self.check = native.check
        self.severity = "warning" if native.severity == _native.SEVERITY_WARNING else "error"
        self.message = _text(native.message)
        self.fix = None
        if native.fix_title is not None:
            self.fix = (
                _text(native.fix_title),
                _text(native.fix_text),
                native.fix_line,
                native.fix_column,
                native.fix_end_line,
                native.fix_end_column,
            )

    def __str__(self):
        where = f"{self.file}:{self.line}:{self.column}" if self.line > 0 else self.file
        check = f" [check {self.check}]" if self.check > 0 else ""
        return f"{where}: {self.severity}: {self.message}{check}"

    def __repr__(self):
        return f"<Diagnostic {self}>"


# ---- Loading ----------------------------------------------------------------------------------


def load(path):
    """Load a definition from a file. Raises LoadError, carrying every diagnostic, if it does not load."""
    return _finish(_native.framedsl_load_file(os.fsencode(path)))


def loads(source, name=None, path=None):
    """Load a definition from text.

    `name` is what diagnostics are attributed to. `path`, instead, loads the text as though it were
    the file at `path` -- diagnostics say that path, and imports resolve from its directory -- which
    is framedsl_load_source(); the file need not exist.
    """
    text = source.encode("utf-8") if isinstance(source, str) else bytes(source)
    if path is not None:
        return _finish(_native.framedsl_load_source(text, os.fsencode(path)))
    return _finish(_native.framedsl_load_string(text, None if name is None else name.encode("utf-8")))


def _finish(result):
    if not result:
        raise MemoryError("framedsl could not allocate a load result")
    source_name = _text(_native.framedsl_source_name(result))
    diagnostics = [
        Diagnostic(_native.framedsl_diagnostic(result, i).contents, source_name)
        for i in range(_native.framedsl_diagnostic_count(result))
    ]
    if not _native.framedsl_ok(result):
        status = _native.framedsl_status(result)
        _native.framedsl_result_free(result)
        if status == _native.ERROR_MEMORY:
            raise MemoryError(f"out of memory loading {source_name}")
        lines = "\n".join(str(d) for d in diagnostics) or f"{source_name}: could not be loaded"
        raise LoadError(lines, status, diagnostics)
    return Definition(result, diagnostics)


# ---- Definitions ------------------------------------------------------------------------------


class Definition:
    """A loaded definition. Make frames from it with frame(); it lives as long as any of them do."""

    def __init__(self, result, diagnostics):
        self._result = result
        self._handle = _native.framedsl_definition(result)
        # Warnings, on a definition that loaded: a load that succeeds may still have something to say.
        self.diagnostics = diagnostics
        # Dotted name -> (FieldId, is_scalar). framedsl_find() is a string walk; a script setting the
        # same field in a loop should pay for it once, as a C program resolving its ids up front does.
        self._ids = {}

    def __del__(self):
        result, self._result = getattr(self, "_result", None), None
        if result:
            _native.framedsl_result_free(result)

    def frame(self):
        """A new frame: every field zero, one allocation, reused for as long as it is kept."""
        return Frame(self)

    @property
    def packets(self):
        """Every packet's name, in declaration order."""
        count = _native.framedsl_packet_count(self._handle)
        return [_text(_native.framedsl_packet_name(self._handle, i)) for i in range(count)]

    def fields(self, packet):
        """The names of `packet`'s fields, in the order they go on the wire."""
        index = ctypes.c_size_t()
        if not _native.framedsl_find_packet(self._handle, packet.encode("utf-8"), ctypes.byref(index)):
            raise FieldError(f"no packet {packet!r}")
        names = []
        for i in range(_native.framedsl_field_count(self._handle, index.value)):
            field = _native.Field()
            _native.framedsl_field(self._handle, index.value, i, ctypes.byref(field))
            names.append(_text(field.name))
        return names

    @property
    def calibrations(self):
        """Every calibration's name, in declaration order."""
        count = _native.framedsl_cal_count(self._handle)
        return [_text(_native.framedsl_cal_name(self._handle, i)) for i in range(count)]

    @property
    def enums(self):
        """Every enum, in declaration order: {enum name: {name: value}}, each in increasing value."""
        enums = {}
        for i in range(_native.framedsl_enum_count(self._handle)):
            names = {}
            for j in range(_native.framedsl_enum_item_count(self._handle, i)):
                name = ctypes.c_char_p()
                value = ctypes.c_uint64()
                _native.framedsl_enum_item(self._handle, i, j, ctypes.byref(name), ctypes.byref(value))
                names[_text(name.value)] = value.value
            enums[_text(_native.framedsl_enum_name(self._handle, i))] = names
        return enums

    def calibrate(self, name, raw):
        """Run calibration `name` forward: raw value in, engineering value out."""
        out = ctypes.c_double()
        _check(_native.framedsl_calibrate(self._handle, self._cal(name), raw, ctypes.byref(out)), name)
        return out.value

    def uncalibrate(self, name, eng):
        """Run calibration `name` backward, unrounded. FieldError if the curve has no inverse."""
        out = ctypes.c_double()
        _check(_native.framedsl_uncalibrate(self._handle, self._cal(name), eng, ctypes.byref(out)), name)
        return out.value

    def emit_c(self, prefix=None):
        """Every calibration as a C header, as `frame-dsl --emit-c` writes it."""
        encoded = None if prefix is None else prefix.encode("utf-8")
        length = _native.framedsl_emit_c(self._handle, encoded, None, 0)
        buffer = ctypes.create_string_buffer(length + 1)
        _native.framedsl_emit_c(self._handle, encoded, buffer, length + 1)
        return buffer.value.decode("utf-8")

    def _cal(self, name):
        cal = _native.CalId()
        if not _native.framedsl_find_cal(self._handle, name.encode("utf-8"), ctypes.byref(cal)):
            raise FieldError(f"no calibration {name!r}")
        return cal

    def _field(self, dotted):
        """(FieldId, is_scalar) for a dotted name, or FieldError."""
        known = self._ids.get(dotted)
        if known is not None:
            return known
        field_id = _native.FieldId()
        if not _native.framedsl_find(self._handle, dotted.encode("utf-8"), ctypes.byref(field_id)):
            raise FieldError(f"no field {dotted!r}")
        known = (field_id, self._is_scalar(field_id))
        self._ids[dotted] = known
        return known

    def _is_scalar(self, field_id):
        info = _native.Field()
        if field_id.member == 0:
            _native.framedsl_field(self._handle, field_id.packet, field_id.field, ctypes.byref(info))
        else:
            _native.framedsl_member(
                self._handle, field_id.packet, field_id.field, field_id.member - 1, ctypes.byref(info)
            )
        return info.is_scalar


# ---- Frames -----------------------------------------------------------------------------------


class Frame:
    """One frame's worth of values: set fields, encode; or decode, read fields.

    frame["PKT.field"] is the raw value: an int for a scalar, bytes for anything wider. Elements of a
    repetition are "PKT.samples[3].reading". frame.eng["PKT.field"] is a calibrated field's value in
    engineering units, and frame.names["PKT.field"] a field typed by an enum, by name.
    """

    def __init__(self, definition):
        self._definition = definition  # Kept: the frame must not outlive the definition it points into.
        self._handle = _native.framedsl_frame_new(definition._handle)
        if not self._handle:
            raise MemoryError("framedsl could not allocate a frame")

    def __del__(self):
        handle, self._handle = getattr(self, "_handle", None), None
        if handle:
            _native.framedsl_frame_free(handle)

    @property
    def definition(self):
        return self._definition

    @property
    def eng(self):
        """Calibrated fields in engineering units: frame.eng["PKT.field"], read and written."""
        return _Engineering(self)

    @property
    def names(self):
        """Fields typed by an enum, by name: frame.names["PKT.field"], read and written."""
        return _Names(self)

    def reset(self):
        """Every field back to zero, as a new frame is."""
        _native.framedsl_frame_reset(self._handle)

    def __getitem__(self, dotted):
        field_id, is_scalar = self._definition._field(dotted)
        return self._get(field_id, is_scalar, dotted)

    def __setitem__(self, dotted, value):
        field_id, is_scalar = self._definition._field(dotted)
        if is_scalar:
            if isinstance(value, bool) or not isinstance(value, int):
                raise TypeError(f"{dotted} is a scalar and takes an int, not {type(value).__name__}")
            if value < 0 or value >= 1 << 64:
                raise RangeError(f"{dotted}: {value} is not an unsigned 64-bit value")
            _check(_native.framedsl_set_u64(self._handle, field_id, value), f"{dotted} = {value}")
        else:
            if not isinstance(value, (bytes, bytearray, memoryview)):
                raise TypeError(f"{dotted} is a bit string and takes bytes, not {type(value).__name__}")
            data = bytes(value)
            _check(_native.framedsl_set_bytes(self._handle, field_id, data, len(data)), f"{dotted}: {len(data)} bytes")

    def count(self, dotted):
        """How many elements the repetition `dotted` (or any member of it) holds in this frame."""
        field_id, _ = self._definition._field(dotted)
        count = ctypes.c_size_t()
        _check(_native.framedsl_count(self._handle, field_id, ctypes.byref(count)), dotted)
        return count.value

    def elements(self, dotted):
        """One member of every element, as a list: frame.elements("TM.samples.reading").

        The member is named through the repetition with no element chosen; the list is as long as
        count() says the frame holds, and each value is what frame["TM.samples[i].reading"] reads.
        """
        field_id, is_scalar = self._definition._field(dotted)
        element = _native.FieldId()
        values = []
        for i in range(self.count(dotted)):
            _check(_native.framedsl_element(self._definition._handle, field_id, i, ctypes.byref(element)), dotted)
            values.append(self._get(element, is_scalar, f"{dotted}[{i}]"))
        return values

    def encode(self, packet):
        """The frame as `packet`, as bytes."""
        length = ctypes.c_size_t()
        status = ctypes.c_int(_native.OK)
        data = _native.framedsl_encode(self._handle, packet.encode("utf-8"), ctypes.byref(length), ctypes.byref(status))
        if not data:
            _raise(status.value, f"encoding {packet}")
        return ctypes.string_at(data, length.value)

    def decode(self, data):
        """Read one frame from the start of `data` and return its packet's name.

        Bytes past the end of the frame are ignored, as in C; decode_prefix() says how many were used.
        A failed decode raises and leaves every field as it was.
        """
        return self.decode_prefix(data)[0]

    def decode_prefix(self, data):
        """Read one frame from the start of `data`: (packet name, bytes it took). For walking a stream."""
        data = bytes(data)
        consumed = ctypes.c_size_t()
        status = ctypes.c_int(_native.OK)
        name = _native.framedsl_decode(self._handle, data, len(data), ctypes.byref(consumed), ctypes.byref(status))
        if name is None:
            _raise(status.value, f"decoding {len(data)} bytes")
        return name.decode("utf-8"), consumed.value

    def _get(self, field_id, is_scalar, what):
        if is_scalar:
            value = ctypes.c_uint64()
            _check(_native.framedsl_get_u64(self._handle, field_id, ctypes.byref(value)), what)
            return value.value
        length = ctypes.c_size_t()
        status = ctypes.c_int(_native.OK)
        data = _native.framedsl_get_bytes(self._handle, field_id, ctypes.byref(length), ctypes.byref(status))
        if not data:
            _raise(status.value, what)
        return ctypes.string_at(data, length.value)


class _Engineering:
    """frame.eng: calibrated fields in engineering units, through framedsl_get_eng() and _set_eng()."""

    __slots__ = ("_frame",)

    def __init__(self, frame):
        self._frame = frame

    def __getitem__(self, dotted):
        frame = self._frame
        field_id, _ = frame._definition._field(dotted)
        value = ctypes.c_double()
        _check(_native.framedsl_get_eng(frame._handle, field_id, ctypes.byref(value)), dotted)
        return value.value

    def __setitem__(self, dotted, value):
        frame = self._frame
        field_id, _ = frame._definition._field(dotted)
        _check(_native.framedsl_set_eng(frame._handle, field_id, float(value)), f"{dotted} = {value}")


class _Names:
    """frame.names: fields typed by an enum, by name, through framedsl_get_name() and _set_name().

    Reading a value the enum does not list raises UnlistedError; frame[...] still reads the number.
    """

    __slots__ = ("_frame",)

    def __init__(self, frame):
        self._frame = frame

    def __getitem__(self, dotted):
        frame = self._frame
        field_id, _ = frame._definition._field(dotted)
        name = ctypes.c_char_p()
        _check(_native.framedsl_get_name(frame._handle, field_id, ctypes.byref(name)), dotted)
        return _text(name.value)

    def __setitem__(self, dotted, name):
        frame = self._frame
        field_id, _ = frame._definition._field(dotted)
        if not isinstance(name, str):
            raise TypeError(f"{dotted} takes a name, not {type(name).__name__}")
        _check(_native.framedsl_set_name(frame._handle, field_id, name.encode("utf-8")), f"{dotted} = {name}")
