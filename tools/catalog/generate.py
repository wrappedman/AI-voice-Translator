#!/usr/bin/env python3
"""Regenerate catalog.json from the compiled-in PACKS table.

The C table in src/app/osai_app.c is the single source of truth; this script
mirrors it into the human-readable release manifest so the two cannot drift.

    python3 tools/catalog/generate.py
"""
import json
import os
import re
import sys
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "src", "app", "osai_app.c")
OUT = os.path.join(ROOT, "tools", "catalog", "catalog.json")

ROW = re.compile(
    r'\{"(?P<id>[^"]+)","(?P<name>[^"]+)","(?P<kind>[^"]+)","(?P<language>[^"]*)",'
    r'"(?P<license>[^"]+)",\s*"(?P<url>[^"]+)",\s*"(?P<sha256>[0-9a-f]{64})",'
    r'\s*"(?P<notes>[^"]*)","(?P<ext>[^"]+)",(?P<langs>NULL|QWEN_LANGS),'
    r'(?P<size>\d+),(?P<ram>\d+)\}')

ENGINE = {"asr": "whisper.cpp", "mt": "llama.cpp", "lang": "fasttext"}
HOMEPAGE = {"asr": "https://github.com/ggerganov/whisper.cpp",
            "mt": "https://github.com/ggml-org/llama.cpp",
            "lang": "https://fasttext.cc/docs/en/language-identification.html"}


def main() -> int:
    with open(APP, encoding="utf-8") as f:
        src = f.read()
    start = src.index("static const pack_def PACKS[] = {")
    end = src.index("};", start)
    rows = list(ROW.finditer(src[start:end]))
    if not rows:
        print("no PACKS rows matched; update the regex in generate.py")
        return 1
    models = []
    for m in rows:
        d = m.groupdict()
        home = HOMEPAGE[d["kind"]]
        if "glotlid" in d["id"]:
            home = "https://huggingface.co/cis-lmu/glotlid"
        elif "qwen" in d["id"]:
            home = "https://huggingface.co/Qwen"
        models.append({
            "id": d["id"], "name": d["name"], "kind": d["kind"],
            "language": d["language"], "engine": ENGINE[d["kind"]],
            "license": d["license"], "homepage": home,
            "size_bytes": int(d["size"]), "sha256": d["sha256"], "url": d["url"],
            "status": "available", "min_ram_mb": int(d["ram"]),
            "notes": d["notes"], "verified": "sha256",
        })
    doc = {
        "schema": 1,
        "updated": date.today().isoformat(),
        "note": ("No models are bundled. Every entry is a user-initiated HTTPS "
                 "download from the official upstream, pinned by exact size and "
                 "SHA-256. min_ram_mb is the approximate resident memory while "
                 "the model runs (shown in the UI). Installation registers the "
                 "model inactive; activation is a separate explicit step."),
        "models": models,
    }
    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote %s (%d entries)" % (OUT, len(models)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
