"""The C API, one call for one call, through ctypes.

Nothing here is Pythonic on purpose. Every function in include/framedsl.h that the package uses is
declared below with its argument and result types, under its own name, so this module can be read
against the header line by line and tested on its own -- a wrong signature shows up here as a wrong
value, not three layers up as a strange one. framedsl/__init__.py builds the public API on top.

The structs mirror the header's field for field and in the same order. ctypes lays a Structure out
the way the C compiler lays out the struct it copies, so a field added to the header and not here
shifts every field after it. tests/test_native.py reads a known definition's fields and diagnostics
through these structs and checks every member, so a layout that has drifted fails there.
"""

import ctypes
import os

# ---- Finding the library ----------------------------------------------------------------------

# The soname first: it is what `make install` and a distribution's package put on the loader path. The
# unversioned name is the development link, there only where somebody installed the headers too.
_NAMES = ("libframedsl.so.0", "libframedsl.so")


def _load_library():
    """FRAMEDSL_LIBRARY if set, then a copy next to this package, then the system loader's path.

    FRAMEDSL_LIBRARY is final when it is set: a path somebody names on purpose that does not load is
    an error to report, not a hint to try something else and quietly get a different library.
    """
    tried = []

    explicit = os.environ.get("FRAMEDSL_LIBRARY")
    if explicit:
        try:
            return ctypes.CDLL(explicit)
        except OSError as error:
            raise ImportError(f"FRAMEDSL_LIBRARY={explicit} could not be loaded: {error}") from error
    tried.append("FRAMEDSL_LIBRARY (not set)")

    here = os.path.dirname(os.path.abspath(__file__))
    for name in _NAMES:
        candidate = os.path.join(here, name)
        if os.path.exists(candidate):
            return ctypes.CDLL(candidate)
    tried.append(f"a copy next to the package ({os.path.join(here, _NAMES[0])}: not there; `make python` puts one)")

    for name in _NAMES:
        try:
            return ctypes.CDLL(name)
        except OSError:
            pass
    tried.append(f"the system loader's path ({' or '.join(_NAMES)}: not found; `make install` puts one)")

    raise ImportError("libframedsl could not be found. Looked in:\n  - " + "\n  - ".join(tried))


lib = _load_library()

# ---- Constants and enums ----------------------------------------------------------------------

SCALAR_MAX_BITS = 64
NO_ELEMENT = 0xFFFFFFFF

OK = 0
ERROR_IO = 1
ERROR_INVALID = 2
ERROR_MEMORY = 3
ERROR_FIELD = 4
ERROR_RANGE = 5
ERROR_BUFFER = 6
ERROR_UNRECOGNISED = 7
ERROR_TRUNCATED = 8

BIT_ORDER_MSB_FIRST = 0
BIT_ORDER_LSB_FIRST = 1

BYTE_ORDER_BIG_ENDIAN = 0
BYTE_ORDER_LITTLE_ENDIAN = 1

SEVERITY_ERROR = 0
SEVERITY_WARNING = 1

# ---- Structs ----------------------------------------------------------------------------------


class Diagnostic(ctypes.Structure):
    _fields_ = [
        ("line", ctypes.c_int),
        ("column", ctypes.c_int),
        ("check", ctypes.c_int),
        ("message", ctypes.c_char_p),
        ("file", ctypes.c_char_p),
        ("severity", ctypes.c_int),
        ("end_line", ctypes.c_int),
        ("end_column", ctypes.c_int),
        ("fix_title", ctypes.c_char_p),
        ("fix_text", ctypes.c_char_p),
        ("fix_line", ctypes.c_int),
        ("fix_column", ctypes.c_int),
        ("fix_end_line", ctypes.c_int),
        ("fix_end_column", ctypes.c_int),
    ]


class Field(ctypes.Structure):
    _fields_ = [
        ("name", ctypes.c_char_p),
        ("variable", ctypes.c_bool),
        ("width_bits", ctypes.c_uint64),
        ("max_bits", ctypes.c_uint64),
        ("reserved_bits", ctypes.c_uint64),
        ("offset_known", ctypes.c_bool),
        ("offset_bits", ctypes.c_uint64),
        ("is_scalar", ctypes.c_bool),
        ("has_fixed", ctypes.c_bool),
        ("fixed", ctypes.c_uint64),
        ("bit_order", ctypes.c_int),
        ("byte_order", ctypes.c_int),
        ("cal", ctypes.c_char_p),
        ("cal_invertible", ctypes.c_bool),
        ("element_bits", ctypes.c_uint64),
        ("member_count", ctypes.c_size_t),
    ]


class FieldId(ctypes.Structure):
    _fields_ = [
        ("owner", ctypes.c_uint32),
        ("packet", ctypes.c_uint16),
        ("field", ctypes.c_uint16),
        ("element", ctypes.c_uint32),
        ("member", ctypes.c_uint16),
    ]


class CalId(ctypes.Structure):
    _fields_ = [
        ("owner", ctypes.c_uint32),
        ("index", ctypes.c_uint32),
    ]


# Opaque handles: only ever pointers, so a void pointer each. Distinct names, so a signature below
# says which one it takes.
Result = ctypes.c_void_p
Definition = ctypes.c_void_p
Frame = ctypes.c_void_p

# ---- Functions --------------------------------------------------------------------------------

_bytes_p = ctypes.POINTER(ctypes.c_uint8)
_size_p = ctypes.POINTER(ctypes.c_size_t)
_status_p = ctypes.POINTER(ctypes.c_int)


def _declare(name, restype, *argtypes):
    function = getattr(lib, name)
    function.restype = restype
    function.argtypes = list(argtypes)
    globals()[name] = function


# Loading.
_declare("framedsl_load_file", Result, ctypes.c_char_p)
_declare("framedsl_load_string", Result, ctypes.c_char_p, ctypes.c_char_p)
_declare("framedsl_load_source", Result, ctypes.c_char_p, ctypes.c_char_p)
_declare("framedsl_result_free", None, Result)
_declare("framedsl_status", ctypes.c_int, Result)
_declare("framedsl_ok", ctypes.c_bool, Result)
_declare("framedsl_source_name", ctypes.c_char_p, Result)
_declare("framedsl_diagnostic_count", ctypes.c_size_t, Result)
_declare("framedsl_diagnostic", ctypes.POINTER(Diagnostic), Result, ctypes.c_size_t)
_declare("framedsl_definition", Definition, Result)

# Introspection.
_declare("framedsl_packet_count", ctypes.c_size_t, Definition)
_declare("framedsl_packet_name", ctypes.c_char_p, Definition, ctypes.c_size_t)
_declare("framedsl_packet_reserved_bits", ctypes.c_uint64, Definition, ctypes.c_size_t)
_declare("framedsl_find_packet", ctypes.c_bool, Definition, ctypes.c_char_p, _size_p)
_declare("framedsl_field_count", ctypes.c_size_t, Definition, ctypes.c_size_t)
_declare("framedsl_field", ctypes.c_bool, Definition, ctypes.c_size_t, ctypes.c_size_t, ctypes.POINTER(Field))
_declare("framedsl_find_field", ctypes.c_bool, Definition, ctypes.c_size_t, ctypes.c_char_p, _size_p)
_declare(
    "framedsl_member",
    ctypes.c_bool,
    Definition,
    ctypes.c_size_t,
    ctypes.c_size_t,
    ctypes.c_size_t,
    ctypes.POINTER(Field),
)

# Frames.
_declare("framedsl_frame_new", Frame, Definition)
_declare("framedsl_frame_free", None, Frame)
_declare("framedsl_frame_reset", None, Frame)
_declare("framedsl_find", ctypes.c_bool, Definition, ctypes.c_char_p, ctypes.POINTER(FieldId))
_declare("framedsl_element", ctypes.c_int, Definition, FieldId, ctypes.c_size_t, ctypes.POINTER(FieldId))
_declare("framedsl_count", ctypes.c_int, Frame, FieldId, _size_p)
_declare("framedsl_field_id", ctypes.c_bool, Definition, ctypes.c_size_t, ctypes.c_size_t, ctypes.POINTER(FieldId))

# Values.
_declare("framedsl_set_u64", ctypes.c_int, Frame, FieldId, ctypes.c_uint64)
_declare("framedsl_get_u64", ctypes.c_int, Frame, FieldId, ctypes.POINTER(ctypes.c_uint64))
_declare("framedsl_set_bytes", ctypes.c_int, Frame, FieldId, ctypes.c_char_p, ctypes.c_size_t)
_declare("framedsl_get_bytes", _bytes_p, Frame, FieldId, _size_p, _status_p)
_declare("framedsl_get_bytes_into", ctypes.c_int, Frame, FieldId, ctypes.c_char_p, ctypes.c_size_t, _size_p)
_declare("framedsl_byte_length", ctypes.c_size_t, Frame, FieldId)

# Encoding and decoding.
_declare("framedsl_encode", _bytes_p, Frame, ctypes.c_char_p, _size_p, _status_p)
_declare("framedsl_encode_into", ctypes.c_int, Frame, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, _size_p)
_declare("framedsl_decode", ctypes.c_char_p, Frame, ctypes.c_char_p, ctypes.c_size_t, _size_p, _status_p)

# Calibrations.
_declare("framedsl_get_eng", ctypes.c_int, Frame, FieldId, ctypes.POINTER(ctypes.c_double))
_declare("framedsl_set_eng", ctypes.c_int, Frame, FieldId, ctypes.c_double)
_declare("framedsl_cal_count", ctypes.c_size_t, Definition)
_declare("framedsl_cal_name", ctypes.c_char_p, Definition, ctypes.c_size_t)
_declare("framedsl_find_cal", ctypes.c_bool, Definition, ctypes.c_char_p, ctypes.POINTER(CalId))
_declare("framedsl_calibrate", ctypes.c_int, Definition, CalId, ctypes.c_double, ctypes.POINTER(ctypes.c_double))
_declare("framedsl_uncalibrate", ctypes.c_int, Definition, CalId, ctypes.c_double, ctypes.POINTER(ctypes.c_double))
_declare("framedsl_emit_c", ctypes.c_size_t, Definition, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t)

# framedsl_format() returns memory the caller frees with free(), so the C library's own free() is
# declared too: releasing it through anything else is undefined.
_declare("framedsl_format", ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p))
_libc = ctypes.CDLL(None)
free = _libc.free
free.restype = None
free.argtypes = [ctypes.c_void_p]
