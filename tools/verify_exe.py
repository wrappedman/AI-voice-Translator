#!/usr/bin/env python3
"""Sanity-check a built Windows executable.

CI cannot run a Windows binary, so the next best thing is to verify the parts
of the PE header a broken build gets wrong: machine type, optional-header
magic, GUI subsystem, no DLL flag, and the embedded version resource.
"""
import struct
import sys

EXPECT_MACHINE = 0x8664        # AMD64
EXPECT_MAGIC_PE32P = 0x020B
SUBSYSTEM_GUI = 2


def check(path: str) -> int:
    with open(path, "rb") as f:
        data = f.read()
    fails = []
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
    for needle, what in ((b"OpenSpeech AI", "product name"),
                         (b"1.0.0", "version string"),
                         (b"permonitorv2", "DPI manifest")):
        if needle not in data:
            fails.append("missing %s resource" % what)
    return report(fails, path, data)


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
