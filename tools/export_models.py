#!/usr/bin/env python3
"""Write the models the release ships into dist/models/.

The application builds its two builtin models on first start. That is correct
for a machine that already has the program, but it is not what "the folder
contains everything the program needs" means: a fresh copy of dist/ has to work
with no prior state and no network.

This script runs the headless binary against a throwaway data folder and copies
the containers it writes into dist/models/. make_dist.py then ships them next to
the exe, and the Windows build seeds them into the data folder on first start.

    python3 tools/export_models.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADLESS = os.path.join(ROOT, "build", "openspeech")
OUT = os.path.join(ROOT, "dist", "models")

# The containers the first start creates, and the name each ships under.
WANTED = {
    "builtin-asr-fa.osam": "builtin-asr-fa.osam",
    "builtin-mt.osmt": "builtin-mt.osmt",
}


def main() -> int:
    if not os.path.exists(HEADLESS):
        print("missing %s - run `make linux` first" % HEADLESS, file=sys.stderr)
        return 1
    os.makedirs(OUT, exist_ok=True)
    started = 0
    with tempfile.TemporaryDirectory(prefix="osai-export-") as tmp:
        env = dict(os.environ, OSAI_DATA_DIR=tmp)
        for args in (["--models-json"], ["--selftest"]):
            r = subprocess.run([HEADLESS] + args, env=env, cwd=ROOT,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            if r.returncode != 0:
                print("headless run failed (%s): %s" % (" ".join(args),
                                                       r.stderr.decode("utf-8", "replace")[:400]),
                      file=sys.stderr)
                return 1
            started += 1
        models = os.path.join(tmp, "Models")   # the data folder names are capitalised
        found = sorted(os.listdir(models)) if os.path.isdir(models) else []
        for name, arc in WANTED.items():
            src = os.path.join(models, name)
            if not os.path.exists(src):
                print("the build did not produce %s (found: %s)" % (name, ", ".join(found)),
                      file=sys.stderr)
                return 1
            shutil.copy2(src, os.path.join(OUT, arc))
            print("model : %-22s %8d bytes" % (arc, os.path.getsize(src)))
    print("wrote %s (%d headless runs)" % (OUT, started))
    return 0


if __name__ == "__main__":
    sys.exit(main())
