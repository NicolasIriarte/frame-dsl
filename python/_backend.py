"""A PEP 517 build backend for this one package, in the standard library alone.

pip installs a project's backend into an isolated environment before building it, and every usual
backend (setuptools, flit, hatchling) is a download. This builds the one wheel `pip install ./python`
needs -- framedsl/*.py, plus the copy of libframedsl that `make python` puts next to them -- and an
sdist, and nothing else: no editable installs, no extensions, no configuration beyond pyproject.toml's
name and version.

A wheel that carries the library is tagged for this platform, since it holds x86-64 Linux code; one
without it is pure Python and finds the library at run time (FRAMEDSL_LIBRARY or the system loader).
"""

import base64
import hashlib
import io
import os
import sysconfig
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGE = "framedsl"


def _project():
    """name and version from pyproject.toml, read by hand: tomllib is 3.11+, and this is two keys."""
    values = {}
    section = None
    with open(os.path.join(HERE, "pyproject.toml")) as handle:
        for line in handle:
            line = line.split("#", 1)[0].strip()
            if line.startswith("["):
                section = line
            elif section == "[project]" and "=" in line:
                key, value = (part.strip() for part in line.split("=", 1))
                if key in ("name", "version", "requires-python", "description"):
                    values[key] = value.strip('"')
    return values


def _files():
    """What goes in the wheel, as (path in the archive, path on disk)."""
    directory = os.path.join(HERE, PACKAGE)
    for name in sorted(os.listdir(directory)):
        if name.endswith(".py") or name.startswith("libframedsl.so"):
            yield f"{PACKAGE}/{name}", os.path.join(directory, name)


def _metadata(project):
    return (
        "Metadata-Version: 2.1\n"
        f"Name: {project['name']}\n"
        f"Version: {project['version']}\n"
        f"Summary: {project.get('description', '')}\n"
        f"Requires-Python: {project.get('requires-python', '')}\n"
    )


def _record_line(path, data):
    digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
    return f"{path},sha256={digest},{len(data)}\n"


def get_requires_for_build_wheel(config_settings=None):
    return []


def get_requires_for_build_sdist(config_settings=None):
    return []


def build_wheel(wheel_directory, config_settings=None, metadata_directory=None):
    project = _project()
    files = list(_files())
    carries_library = any(os.path.basename(path).startswith("libframedsl.so") for path, _ in files)
    if carries_library:
        tag = "py3-none-" + sysconfig.get_platform().replace("-", "_").replace(".", "_")
    else:
        tag = "py3-none-any"

    dist_info = f"{project['name']}-{project['version']}.dist-info"
    wheel_name = f"{project['name']}-{project['version']}-{tag}.whl"
    record = ""
    with zipfile.ZipFile(os.path.join(wheel_directory, wheel_name), "w", zipfile.ZIP_DEFLATED) as wheel:
        for archive_path, disk_path in files:
            with open(disk_path, "rb") as handle:
                data = handle.read()
            wheel.writestr(archive_path, data)
            record += _record_line(archive_path, data)
        extra = {
            f"{dist_info}/METADATA": _metadata(project).encode(),
            f"{dist_info}/WHEEL": (
                "Wheel-Version: 1.0\n"
                "Generator: framedsl-backend\n"
                f"Root-Is-Purelib: {'false' if carries_library else 'true'}\n"
                f"Tag: {tag}\n"
            ).encode(),
        }
        for archive_path, data in extra.items():
            wheel.writestr(archive_path, data)
            record += _record_line(archive_path, data)
        wheel.writestr(f"{dist_info}/RECORD", record + f"{dist_info}/RECORD,,\n")
    return wheel_name


def build_sdist(sdist_directory, config_settings=None):
    project = _project()
    base = f"{project['name']}-{project['version']}"
    name = f"{base}.tar.gz"
    with tarfile.open(os.path.join(sdist_directory, name), "w:gz") as sdist:
        for path in ("pyproject.toml", "_backend.py"):
            sdist.add(os.path.join(HERE, path), f"{base}/{path}")
        for archive_path, disk_path in _files():
            if archive_path.endswith(".py"):
                sdist.add(disk_path, f"{base}/{archive_path}")
        pkg_info = _metadata(project).encode()
        info = tarfile.TarInfo(f"{base}/PKG-INFO")
        info.size = len(pkg_info)
        sdist.addfile(info, io.BytesIO(pkg_info))
    return name
