"""What a package built from python/ hands out: the license it declares, in the wheel and the sdist.

MIT's one condition is that the notice stays with copies of the code, so these build the real
archives through the backend, and read the license back out of them.
"""

import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

import _backend

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")


def license_text():
    with open(os.path.join(ROOT, "LICENSE"), "rb") as handle:
        return handle.read()


def unpacked_sdist(directory):
    name = _backend.build_sdist(directory)
    with tarfile.open(os.path.join(directory, name)) as sdist:
        sdist.extractall(directory)
    return os.path.join(directory, name[: -len(".tar.gz")])


def wheel_from(unpacked):
    """Builds a wheel the way pip does from an sdist: its own backend, away from any checkout."""
    return subprocess.run(
        [sys.executable, "-c", "import _backend; print(_backend.build_wheel('.'))"],
        cwd=unpacked,
        env={**os.environ, "PYTHONPATH": unpacked},
        capture_output=True,
        text=True,
    )


class Wheel(unittest.TestCase):
    def test_it_holds_the_license_and_declares_it(self):
        with tempfile.TemporaryDirectory() as directory:
            name = _backend.build_wheel(directory)
            with zipfile.ZipFile(os.path.join(directory, name)) as wheel:
                metadata_path = next(n for n in wheel.namelist() if n.endswith("/METADATA"))
                info = metadata_path.rsplit("/", 1)[0]
                self.assertEqual(wheel.read(f"{info}/licenses/LICENSE"), license_text())
                metadata = wheel.read(f"{info}/METADATA").decode()
                record = wheel.read(f"{info}/RECORD").decode()
        self.assertIn("Metadata-Version: 2.4\n", metadata)
        self.assertIn("License-Expression: MIT\n", metadata)
        self.assertIn("License-File: LICENSE\n", metadata)
        self.assertIn(f"{info}/licenses/LICENSE,", record)


class Sdist(unittest.TestCase):
    def test_it_holds_the_license_and_declares_it(self):
        with tempfile.TemporaryDirectory() as directory:
            unpacked = unpacked_sdist(directory)
            with open(os.path.join(unpacked, "LICENSE"), "rb") as handle:
                self.assertEqual(handle.read(), license_text())
            with open(os.path.join(unpacked, "PKG-INFO")) as handle:
                self.assertIn("License-Expression: MIT\n", handle.read())

    def test_a_wheel_built_from_it_finds_the_license_inside_it(self):
        with tempfile.TemporaryDirectory() as directory:
            built = wheel_from(unpacked_sdist(directory))
            self.assertEqual(built.returncode, 0, built.stderr)

    def test_without_a_license_nothing_is_built(self):
        with tempfile.TemporaryDirectory() as directory:
            unpacked = unpacked_sdist(directory)
            os.remove(os.path.join(unpacked, "LICENSE"))
            built = wheel_from(unpacked)
            self.assertNotEqual(built.returncode, 0)
            self.assertIn("no LICENSE", built.stderr)


if __name__ == "__main__":
    unittest.main()
