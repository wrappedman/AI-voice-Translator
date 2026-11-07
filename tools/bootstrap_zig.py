#!/usr/bin/env python3
"""Fetch a pinned zig into the build cache.

zig ships as a wheel on PyPI, which avoids depending on GitHub releases (and
works in sandboxes that block the objects host). The result is a single
self-contained binary used by `make win`.
"""
import os, shutil, subprocess, sys, tempfile, zipfile, glob

VERSION = "0.17.1"

def main() -> int:
    if len(sys.argv) < 2:
        print("usage: bootstrap_zig.py DEST_DIR", file=sys.stderr)
        return 2
    dest_dir = sys.argv[1]
    target = os.path.join(dest_dir, "zig")
    if os.path.exists(target):
        print("zig already present:", target)
        return 0
    os.makedirs(dest_dir, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        print("downloading ziglang %s from PyPI..." % VERSION)
        rc = subprocess.call([sys.executable, "-m", "pip", "download",
                              "ziglang==" + VERSION, "-d", tmp, "--no-deps", "-q"])
        if rc != 0:
            print("download failed", file=sys.stderr)
            return 1
        wheel = glob.glob(os.path.join(tmp, "*.whl"))
        if not wheel:
            print("no wheel downloaded", file=sys.stderr)
            return 1
        with zipfile.ZipFile(wheel[0]) as z:
            z.extractall(tmp)
        src = os.path.join(tmp, "ziglang", "zig")
        if not os.path.exists(src):
            print("unexpected wheel layout", file=sys.stderr)
            return 1
        # the zig directory holds the binary plus its lib/ tree: copy the whole thing
        lib = os.path.join(tmp, "ziglang", "lib")
        shutil.copy2(src, target)
        os.chmod(target, 0o755)
        if os.path.isdir(lib):
            shutil.copytree(lib, os.path.join(dest_dir, "lib"), dirs_exist_ok=True)
    print("zig written to", target)
    return 0

if __name__ == "__main__":
    sys.exit(main())
