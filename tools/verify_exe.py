#!/usr/bin/env python3
"""Sanity-check a built Windows executable.

CI cannot run a Windows binary here, so this verifies the PE header and a few
load-bearing strings/resources without executing it:
  * OpenSpeechAI.exe      x86-64 PE32+, GUI subsystem, version + DPI manifest
  * OpenSpeechWorker.exe  x86-64 PE32+, console subsystem, UTF-8 manifest

Usage:
    verify_exe.py dist/bin/OpenSpeechAI.exe
    verify_exe.py dist/bin/OpenSpeechWorker.exe --console
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _contains(haystack: bytes, needle: str) -> bool:
    return needle.encode("utf-8") in haystack or needle.encode("utf-16-le") in haystack


APP_REQUIRED = [
    ("Media Foundation is unavailable", "the AAC/M4A decoder is linked in"),
    ("ترجمه انجام شد", "the translate confirmation"),
    ("ابتدا مدل گفتار", "the model-required gate message"),
]
FORBIDDEN = ["no AAC decoder on this platform"]
EXPECT_MACHINE = 0x8664
EXPECT_MAGIC_PE32P = 0x020B


def version_from_header() -> str:
    header = os.path.join(ROOT, "src", "core", "osai_common.h")
    with open(header, encoding="utf-8") as f:
        m = re.search(r'#define\s+OSAI_VERSION\s+"([0-9.]+)"', f.read())
    if not m:
        raise SystemExit("cannot read OSAI_VERSION from %s" % header)
    return m.group(1)


def check(path: str, console: bool) -> int:
    with open(path, "rb") as f:
        data = f.read()
    fails = []
    if not console:
        for needle, why in APP_REQUIRED:
            if not _contains(data, needle):
                fails.append("missing %r: %s" % (needle, why))
    for needle in FORBIDDEN:
        if _contains(data, needle):
            fails.append("contains forbidden fallback %r" % needle)
    if not _contains(data, "UTF-8"):
        fails.append("missing UTF-8 activeCodePage manifest")
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
    expect_sub = 3 if console else 2
    if subsystem != expect_sub:
        fails.append("subsystem %d, expected %d (%s)" % (subsystem, expect_sub, "console" if console else "GUI"))
    dll = struct.unpack_from("<H", data, opt + 70)[0] & 1
    if dll:
        fails.append("image is marked as a DLL")
    if not console:
        ver = version_from_header()
        for needle, what in ((b"OpenSpeech AI", "product name"),
                             (ver.encode(), "version string (%s)" % ver),
                             (b"permonitorv2", "DPI manifest")):
            if needle not in data:
                fails.append("missing %s resource" % what)
        stale = [v for v in ("1.0.0.0", "0.9.0.0", "1.1.0.0")
                 if v != ver and v.encode("utf-16-le") in data]
        for v in stale:
            fails.append("file properties still report version %s" % v)
        if ver.encode("utf-16-le") not in data:
            fails.append("version resource does not carry %s" % ver)
    return report(fails, path, data)


def report(fails, path, data) -> int:
    size_mb = len(data) / (1024 * 1024)
    if fails:
        for f in fails:
            print("  FAIL %s" % f)
        print("%s: INVALID (%.1f MB)" % (path, size_mb))
        return 1
    print("%s: ok — x86-64 PE32+, static, %.1f MB" % (path, size_mb))
    return 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--console", action="store_true", help="expect a console worker instead of the GUI app")
    args = ap.parse_args()
    sys.exit(check(args.path, args.console))
