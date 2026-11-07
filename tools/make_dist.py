#!/usr/bin/env python3
"""Package the Windows build for release.

Produces dist/OpenSpeechAI-<version>-win64.zip containing the exe, the licence
and a short readme, plus SHA-256 sums for both the zip and the bare exe.
"""
import hashlib
import os
import sys
import zipfile

VERSION = "1.0.0"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "dist", "bin", "OpenSpeechAI.exe")
OUT = os.path.join(ROOT, "dist", "OpenSpeechAI-%s-win64.zip" % VERSION)

README = """OpenSpeech AI {ver} — Windows x64
================================

Portable, fully offline speech recognition (Persian + English) and
Persian<->English translation. No installer, no runtime, no network access.

Run
    OpenSpeechAI.exe

The program creates its data folder on first start:
    %LOCALAPPDATA%\\OpenSpeechAI

Everything (models, transcripts, audio copies) stays inside that folder.
Settings are stored in settings.ini next to the data root.

Requirements
    Windows 10 or 11, 64-bit. 350 MB RAM, ~50 MB free disk.

Verified
    SHA-256 of OpenSpeechAI.exe is listed in checksums-{ver}.txt

Licence
    GPL-3.0-or-later. Bundled fonts: Vazirmatn (SIL OFL 1.1), see LICENSE-Vazirmatn.txt
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
    font_lic = os.path.join(ROOT, "src", "ui", "assets", "OFL.txt")
    if os.path.exists(font_lic):
        files.append((font_lic, "LICENSE-Vazirmatn.txt"))

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(EXE, "OpenSpeechAI.exe")
        for src, arc in files:
            z.write(src, arc)
        z.writestr("README.txt", README)

    exe_hash = sha256(EXE)
    zip_hash = sha256(OUT)
    sums = os.path.join(ROOT, "dist", "checksums-%s.txt" % VERSION)
    with open(sums, "w", encoding="utf-8") as f:
        f.write("%s  OpenSpeechAI.exe\n" % exe_hash)
        f.write("%s  %s\n" % (zip_hash, os.path.basename(OUT)))
    print("zip   : %s (%.1f MB)" % (OUT, os.path.getsize(OUT) / 1048576))
    print("sha256: %s" % zip_hash)
    print("exe   : %s" % exe_hash)
    return 0


if __name__ == "__main__":
    sys.exit(main())
