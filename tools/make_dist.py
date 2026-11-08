#!/usr/bin/env python3
"""Package the Windows build for release.

Produces dist/OpenSpeechAI-<version>-win64.zip containing the exe, the licence
and a short readme, plus SHA-256 sums for both the zip and the bare exe.
"""
import hashlib
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version() -> str:
    """The version lives in exactly one place: src/core/osai_common.h.
    The zip name, the readme and the checksum file all come from it, so a
    release cannot end up with a 1.0.0 file name and a 1.1.0 binary."""
    with open(os.path.join(ROOT, "src", "core", "osai_common.h"), encoding="utf-8") as f:
        m = re.search(r'#define\s+OSAI_VERSION\s+"([0-9.]+)"', f.read())
    if not m:
        raise SystemExit("cannot read OSAI_VERSION from src/core/osai_common.h")
    return m.group(1)


VERSION = version()
DIST = os.path.join(ROOT, "dist")
EXE = os.path.join(DIST, "bin", "OpenSpeechAI.exe")
OUT = os.path.join(DIST, "OpenSpeechAI-%s-win64.zip" % VERSION)

# Everything the executable opens at run time. The fonts used to be left out of
# the release, which meant a fresh copy of the zip started with no text at all
# until the user copied src/ui/assets by hand. `make dist` now lays them out next
# to the exe (assets/fonts/…) and zips that whole tree.
ASSET_FILES = [
    ("src/ui/assets/Vazirmatn-Regular.ttf", "assets/fonts/Vazirmatn-Regular.ttf"),
    ("src/ui/assets/Vazirmatn-Medium.ttf", "assets/fonts/Vazirmatn-Medium.ttf"),
    ("src/ui/assets/Vazirmatn-Bold.ttf", "assets/fonts/Vazirmatn-Bold.ttf"),
    ("src/ui/assets/Vazirmatn-OFL.txt", "assets/fonts/OFL-Vazirmatn.txt"),
    ("tools/catalog/catalog.json", "assets/catalog.json"),
    ("README.md", "README.md"),
    ("docs/ARCHITECTURE.md", "docs/ARCHITECTURE.md"),
    ("docs/MODEL_FORMAT.md", "docs/MODEL_FORMAT.md"),
]

README = """OpenSpeech AI {ver} — Windows x64
================================

Portable, fully offline speech recognition (Persian + English) and
Persian<->English translation. No installer, no runtime, no network access.

Run
    OpenSpeechAI.exe

Keep the whole folder together
    OpenSpeechAI.exe needs the assets folder that sits next to it:

        OpenSpeechAI.exe
        assets\\fonts\\Vazirmatn-*.ttf
        assets\\catalog.json

    Move the folder as a unit, or unpack the zip somewhere writable. If the
    fonts are missing the program says so instead of starting blank.

The program creates its data folder on first start:
    %LOCALAPPDATA%\\OpenSpeechAI

Everything (models, transcripts, audio copies) stays inside that folder.
Settings are stored in settings.ini next to the data root.

Requirements
    Windows 10 or 11, 64-bit. 350 MB RAM, ~50 MB free disk.

Verified
    SHA-256 of OpenSpeechAI.exe is listed in checksums-{ver}.txt

Licence
    GPL-3.0-or-later. Bundled fonts: Vazirmatn (SIL OFL 1.1), see
    assets\\fonts\\OFL-Vazirmatn.txt
""".format(ver=VERSION)


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    if not os.path.exists(EXE):
        print("missing %s — run `make win` first" % EXE, file=sys.stderr)
        return 1
    os.makedirs(os.path.join(ROOT, "dist"), exist_ok=True)

    files = []
    licence = os.path.join(ROOT, "LICENSE")
    if os.path.exists(licence):
        files.append((licence, "LICENSE.txt"))

    missing = []
    for src, arc in ASSET_FILES:
        full = os.path.join(ROOT, src.replace("/", os.sep))
        if os.path.exists(full):
            files.append((full, arc))
        elif src.startswith("src/ui/assets/"):
            missing.append(src)
    if missing:
        print("missing required assets: %s" % ", ".join(missing), file=sys.stderr)
        return 1

    # Also lay the tree out under dist/bin so the shipped executable can be run
    # straight out of the repository without unpacking the zip.
    bin_dir = os.path.dirname(EXE)
    for src, arc in ASSET_FILES:
        full = os.path.join(ROOT, src.replace("/", os.sep))
        if not os.path.exists(full):
            continue
        dst = os.path.join(bin_dir, arc.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(full, dst)

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(EXE, "OpenSpeechAI.exe")
        for src, arc in files:
            z.write(src, arc)
        z.writestr("README.txt", README)

    exe_hash = sha256(EXE)
    zip_hash = sha256(OUT)
    sums = os.path.join(ROOT, "dist", "checksums-%s.txt" % VERSION)
    # Paths are relative to dist/, so `cd dist && sha256sum -c checksums-<v>.txt`
    # works. A bare "OpenSpeechAI.exe" is not in that directory (the binary is
    # under bin/), which made the list look correct but verify as missing.
    with open(sums, "w", encoding="utf-8") as f:
        f.write("%s  bin/OpenSpeechAI.exe\n" % exe_hash)
        f.write("%s  %s\n" % (zip_hash, os.path.basename(OUT)))
    print("zip   : %s (%.1f MB)" % (OUT, os.path.getsize(OUT) / 1048576))
    print("sha256: %s" % zip_hash)
    print("exe   : %s" % exe_hash)
    return 0


if __name__ == "__main__":
    sys.exit(main())
