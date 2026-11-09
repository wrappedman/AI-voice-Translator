#!/usr/bin/env python3
"""Sanity-check a built Windows executable.

This verifies the PE header and a few load-bearing strings/resources without
executing the binary. A separate native Windows CI job (tools/win_smoke.py)
runs the shipped worker against a synthetic model in a Persian/spaces folder:
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
    ("درباره نرم‌افزار", "the renamed About section"),
    ("تشخیص گفتار و ترجمه آفلاین", "the offline tagline"),
    ("behnam ehsani", "the developer card"),
    ("inbox.ehsani@gmail.com", "the developer contact"),
    ("cannot start the model worker", "the diagnosable spawn failure"),
    ("worker.log", "worker stderr logging"),
    ("OSAI_WORKER", "the embedded worker resource name"),
    ("همگام‌سازی", "the models-folder sync action"),
    ("پوشه مدل‌ها", "the models-folder setting"),
    ("حذف شود؟", "the two-step delete confirmation"),
    ("درباره سازنده", "the developer section"),
    ("the worker command line is too long", "the MSVCRT argv quoting guard"),
]
WORKER_REQUIRED = [
    ("cannot parse the worker command line", "wide Windows argv parsing"),
    ("cannot open the language model file", "the wide-path fastText loader"),
]
FORBIDDEN = [
    "no AAC decoder on this platform",
    "افزودن مدل از فایل",          # local-file import was removed
    "keep it beside OpenSpeechAI.exe",
]
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
    required = WORKER_REQUIRED if console else APP_REQUIRED
    for needle, why in required:
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
        stale = [v for v in ("1.0.0.0", "0.9.0.0", "1.1.0.0", "1.2.0.0", "1.3.0.0", "1.3.1.0", "1.4.0.0")
                 if v != ver and v.encode("utf-16-le") in data]
        for v in stale:
            fails.append("file properties still report version %s" % v)
        if ver.encode("utf-16-le") not in data:
            fails.append("version resource does not carry %s" % ver)
        # The worker must travel inside the app resource, otherwise activation
        # fails on a machine where only OpenSpeechAI.exe was copied.
        worker = os.path.join(os.path.dirname(os.path.abspath(path)), "OpenSpeechWorker.exe")
        if os.path.exists(worker):
            with open(worker, "rb") as f:
                payload = f.read()
            if payload[:8192] not in data or payload[-8192:] not in data:
                fails.append("the embedded worker payload does not match OpenSpeechWorker.exe")
            else:
                print("  ok   inference worker embedded as RCDATA (%d bytes)" % len(payload))
        else:
            fails.append("OpenSpeechWorker.exe not found next to the app; cannot check the embedded copy")
        print("  ok   subsystem %d (GUI, no console window)" % subsystem)
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
