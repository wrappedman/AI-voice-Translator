#!/usr/bin/env python3
"""Package the Windows build for release.

Produces dist/OpenSpeechAI-<version>-win64.zip containing the GUI executable,
the isolated inference worker, fonts/catalog/docs and SHA-256 sums. No model
weights are bundled: the user downloads and activates them inside the app.
"""
import hashlib
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def version() -> str:
    with open(os.path.join(ROOT, "src", "core", "osai_common.h"), encoding="utf-8") as f:
        m = re.search(r'#define\s+OSAI_VERSION\s+"([0-9.]+)"', f.read())
    if not m:
        raise SystemExit("cannot read OSAI_VERSION from src/core/osai_common.h")
    return m.group(1)


VERSION = version()
DIST = os.path.join(ROOT, "dist")
EXE = os.path.join(DIST, "bin", "OpenSpeechAI.exe")
WORKER = os.path.join(DIST, "bin", "OpenSpeechWorker.exe")
OUT = os.path.join(DIST, "OpenSpeechAI-%s-win64.zip" % VERSION)

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

Portable offline speech recognition and multilingual translation.
No installer, no account, no bundled model weights.

Run
    OpenSpeechAI.exe

Put the folder somewhere writable
    C:\\OpenSpeechAI or your Desktop is fine. Do not run it from inside the
    ZIP, and avoid C:\\Program Files unless you have write access there.

    OpenSpeechAI.exe
    OpenSpeechWorker.exe        (also embedded inside OpenSpeechAI.exe)
    assets\\fonts\\Vazirmatn-*.ttf
    assets\\catalog.json

    The worker runs the neural translation / language-identification engines
    in a separate process. It is embedded in OpenSpeechAI.exe and extracted
    to data\\Cache on first use, so activation works even if you copy the
    exe on its own. Worker diagnostics go to data\\Logs\\worker.log.

First run
    The interface opens in Persian with the light theme.
    Settings -> Models folder chooses where model files live; Sync registers the
    verified files already in that folder. Delete on an installed row removes
    both the file and its database record.
    If a model fails to activate, the message quotes the real reason and
    data\\Logs\\worker.log has the full trace.
    The application starts with NO installed models. Open Models, download
    one speech model (Whisper), one translation model (Qwen GGUF) and one
    language-identification model (fastText LID 176 or GlotLID v4), then activate each.
    Transcription, translation and live features stay disabled until all
    three categories have an active model. The window header always names
    the active model in each category.

    The catalogue offers 8 Whisper variants, 7 Qwen2.5 translation quants and
    6 language-identification models (fastText LID 176 plus GlotLID v4, from
    0.9 MB to 747 MB). Each row states the approximate RAM needed and marks
    light models. Downloads show their percentage, can be cancelled, and are
    verified against pinned sizes and SHA-256 hashes before being installed inactive.
    There is no local-file import: only the official catalogue installs.

Everything the program writes lives next to the executable:
    <folder with OpenSpeechAI.exe>\\data\\
        Models\\  Downloads\\  Database\\  Logs\\  Licenses\\  Audio\\  Cache\\ ...
        settings.ini

Nothing is created in AppData. An older %LOCALAPPDATA%\\OpenSpeechAI profile
is moved into data\\ once on first start, and removed when it is empty.

Requirements
    Windows 10 version 1903 or newer / Windows 11, 64-bit.
    Light models run on 2-4 GB RAM; the Qwen2.5 3B translator recommends 8 GB.
    Several GB of free disk for the models you choose to download.
    Internet access only when you press Download; inference is offline.

Verified
    SHA-256 of OpenSpeechAI.exe, OpenSpeechWorker.exe and the zip are listed
    in checksums-{ver}.txt. Model downloads are verified separately against
    the catalog hashes before installation.

Developer
    behnam ehsani — inbox.ehsani@gmail.com
    https://github.com/wrappedman/AI-voice-Translator

Licence
    GPL-3.0-or-later. Bundled fonts: Vazirmatn (SIL OFL 1.1).
    Third-party engine sources: llama.cpp (MIT), fastText (MIT),
    nlohmann/json (MIT). Model weights have their own licences listed in
    the Models screen and assets/catalog.json.
""".format(ver=VERSION)


def write_rc_header() -> None:
    parts = (VERSION.split(".") + ["0", "0", "0", "0"])[:4]
    if not all(p.isdigit() for p in parts):
        raise SystemExit("OSAI_VERSION must be numeric, got %r" % VERSION)
    path = os.path.join(ROOT, "src", "platform", "win32", "app_version.h")
    body = (
        "/* Generated by tools/make_dist.py from OSAI_VERSION. Do not edit. */\n"
        "#ifndef OSAI_APP_VERSION_RESOURCE_H\n"
        "#define OSAI_APP_VERSION_RESOURCE_H\n"
        "#define OSAI_VER_MAJOR %s\n#define OSAI_VER_MINOR %s\n"
        "#define OSAI_VER_PATCH %s\n#define OSAI_VER_BUILD %s\n"
        "#define OSAI_VERSION_STR \"%s\"\n"
        "#endif\n"
    ) % (parts[0], parts[1], parts[2], parts[3], VERSION)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(body)


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    write_rc_header()
    for binary in (EXE, WORKER):
        if not os.path.exists(binary):
            print("missing %s — run `make win` first" % binary, file=sys.stderr)
            return 1
    os.makedirs(DIST, exist_ok=True)

    files = []
    licence = os.path.join(ROOT, "LICENSE")
    if os.path.exists(licence):
        files.append((licence, "LICENSE.txt"))

    missing = []
    for src, arc in ASSET_FILES:
        full = os.path.join(ROOT, src.replace("/", os.sep))
        if os.path.exists(full):
            files.append((full, arc))
        else:
            missing.append(src)
    if missing:
        print("missing required files: %s" % ", ".join(missing), file=sys.stderr)
        return 1

    bin_dir = os.path.dirname(EXE)
    for src, arc in ASSET_FILES:
        full = os.path.join(ROOT, src.replace("/", os.sep))
        dst = os.path.join(bin_dir, arc.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(full, dst)

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(EXE, "OpenSpeechAI.exe")
        z.write(WORKER, "OpenSpeechWorker.exe")
        for src, arc in files:
            z.write(src, arc)
        z.writestr("README.txt", README)

    exe_hash = sha256(EXE)
    worker_hash = sha256(WORKER)
    zip_hash = sha256(OUT)
    sums = os.path.join(DIST, "checksums-%s.txt" % VERSION)
    with open(sums, "w", encoding="utf-8") as f:
        f.write("%s  bin/OpenSpeechAI.exe\n" % exe_hash)
        f.write("%s  bin/OpenSpeechWorker.exe\n" % worker_hash)
        f.write("%s  %s\n" % (zip_hash, os.path.basename(OUT)))

    absent = [arc for _, arc in ASSET_FILES
              if not os.path.exists(os.path.join(bin_dir, arc.replace("/", os.sep)))]
    if absent:
        print("dist/bin is missing: %s" % ", ".join(absent), file=sys.stderr)
        return 1
    print("layout: dist/bin has exe + worker + %d asset/doc files (no models)" % len(ASSET_FILES))
    print("zip   : %s (%.1f MB)" % (OUT, os.path.getsize(OUT) / 1048576))
    print("sha256: %s" % zip_hash)
    print("exe   : %s" % exe_hash)
    print("worker: %s" % worker_hash)
    return 0


if __name__ == "__main__":
    if "--rc-only" in sys.argv:
        write_rc_header()
        print("version resource: %s" % VERSION)
        sys.exit(0)
    sys.exit(main())
