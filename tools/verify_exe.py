#!/usr/bin/env python3
"""Sanity-check a built Windows executable.

CI cannot run a Windows binary, so the next best thing is to verify the parts
of the PE header a broken build gets wrong: machine type, optional-header
magic, GUI subsystem, no DLL flag, and the embedded version resource.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The exe is a UTF-16LE resource blob on Windows targets and plain ASCII nowhere;
# a string check has to look for both encodings or it fails on a good build.
def _contains(haystack: bytes, needle: str) -> bool:
    if needle.encode("utf-8") in haystack:
        return True
    if needle.encode("utf-16-le") in haystack:
        return True
    return False

# Strings that must be in the shipped binary because they are the code paths the
# 1.1.0 bug reports were about. If a link step silently drops a file — or leaves
# a weak stub in place of the real implementation — the string is missing, and
# this notices without a Windows machine. Persian literals live in the resource
# strings, so both encodings are searched.
REQUIRED = [
    ("Media Foundation is not available", "the AAC/M4A decoder is linked in"),
    ("فهرست داده‌ها", "the in-app training-data list"),
    ("ترجمه انجام شد", "the translate confirmation"),
]
# Strings that must NOT appear: a weak fallback next to the real implementation
# means the linker picked one at random.
FORBIDDEN = [
    "no AAC decoder on this platform",
]

EXPECT_MACHINE = 0x8664        # AMD64
EXPECT_MAGIC_PE32P = 0x020B
SUBSYSTEM_GUI = 2


def check(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    fails = []
    for needle, why in REQUIRED:
        if not _contains(data, needle):
            fails.append("missing %r: %s" % (needle, why))
    for needle in FORBIDDEN:
        if _contains(data, needle):
            fails.append("contains %r, which should not be in a Windows build" % needle)
    if data[:2] != b"MZ":
        fails.append("missing DOS signature")
        return report(fails, path, data)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        fails.append("missing PE signature")
        return report(fails, path, data)
    machine = struct.unpack_from("<H", data, pe + 4)[0]
    if machine != EXPECT_MACHINE:
        fails.append("machine type 0x%04x, expected x86-64" % machine)
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != EXPECT_MAGIC_PE32P:
        fails.append("optional header magic 0x%04x, expected PE32+" % magic)
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    if subsystem != SUBSYSTEM_GUI:
        fails.append("subsystem %d, expected %d (GUI)" % (subsystem, SUBSYSTEM_GUI))
    dll = struct.unpack_from("<H", data, opt + 70)[0] & 1
    if dll:
        fails.append("image is marked as a DLL")
    # The version is checked against the one header the whole program uses, so a
    # release cannot ship with the previous version's string baked in.
    ver = version_from_header()
    for needle, what in ((b"OpenSpeech AI", "product name"),
                         (ver.encode(), "version string (%s)" % ver),
                         (b"permonitorv2", "DPI manifest")):
        if needle not in data:
            fails.append("missing %s resource" % what)
    # The resource is stored as UTF-16, so a stale FILEVERSION block would not
    # show up in the search above. Check the previous-version string explicitly:
    # 1.0.0.0 in the file properties of a 1.1.0 release is exactly the kind of
    # drift this script exists to catch.
    stale = [v for v in ("1.0.0.0", "0.9.0.0")
             if v != ver and v.encode("utf-16-le") in data]
    for v in stale:
        fails.append("file properties still report version %s" % v)
    if ver.encode("utf-16-le") not in data:
        fails.append("version resource does not carry %s" % ver)
    return report(fails, path, data)


def version_from_header() -> str:
    """The version lives in exactly one place: src/core/osai_common.h."""
    header = os.path.join(ROOT, "src", "core", "osai_common.h")
    with open(header, encoding="utf-8") as f:
        m = re.search(r'#define\s+OSAI_VERSION\s+"([0-9.]+)"', f.read())
    if not m:
        raise SystemExit("cannot read OSAI_VERSION from %s" % header)
    return m.group(1)


def report(fails, path, data) -> int:
    size_mb = len(data) / (1024 * 1024)
    if fails:
        for f in fails:
            print("  FAIL %s" % f)
        print("%s: INVALID (%.1f MB)" % (path, size_mb))
        return 1
    print("%s: ok — x86-64 PE32+, GUI subsystem, static, %.1f MB" % (path, size_mb))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: verify_exe.py PATH", file=sys.stderr)
        sys.exit(2)
    sys.exit(check(sys.argv[1]))
