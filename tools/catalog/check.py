#!/usr/bin/env python3
"""Validate the downloadable model catalogue.

No weights are bundled or downloaded by this check. It validates catalog.json
and cross-checks every pin against the compiled-in catalog in
src/app/osai_app.c, so the UI and the release manifest cannot drift apart.

    check.py          structural + source-pin validation
    check.py --hash   same check (kept for CI compatibility; no builtin models)
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOG = os.path.join(ROOT, "tools", "catalog", "catalog.json")
APP = os.path.join(ROOT, "src", "app", "osai_app.c")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
KINDS = {"asr", "mt", "lang"}
ENGINES = {"whisper.cpp", "llama.cpp", "fasttext"}


def fail(errors, msg):
    errors.append(msg)
    print("  FAIL %s" % msg)


def main() -> int:
    errors = []
    if not os.path.exists(CATALOG):
        print("missing", CATALOG)
        return 1
    with open(CATALOG, encoding="utf-8") as f:
        try:
            doc = json.load(f)
        except json.JSONDecodeError as e:
            print("catalog.json is not valid JSON:", e)
            return 1
    with open(APP, encoding="utf-8") as f:
        app_src = f.read()

    print("catalog.json")
    if doc.get("schema") != 1:
        fail(errors, "schema must be 1")
    models = doc.get("models")
    if not isinstance(models, list) or not models:
        fail(errors, "models must be a non-empty array")
        models = []

    seen = set()
    kinds = set()
    for i, m in enumerate(models):
        tag = m.get("id", "#%d" % i)
        if not m.get("id"):
            fail(errors, "entry %d has no id" % i)
            continue
        if tag in seen:
            fail(errors, "duplicate id %s" % tag)
        seen.add(tag)
        kinds.add(m.get("kind"))
        if not m.get("name"):
            fail(errors, "%s has no name" % tag)
        if m.get("kind") not in KINDS:
            fail(errors, "%s kind must be one of %s" % (tag, sorted(KINDS)))
        if m.get("engine") not in ENGINES:
            fail(errors, "%s engine must be one of %s" % (tag, sorted(ENGINES)))
        if m.get("status") != "available":
            fail(errors, "%s must be status=available; nothing is bundled/installed" % tag)
        if not m.get("license"):
            fail(errors, "%s has no license" % tag)
        sha = m.get("sha256", "")
        if not HEX64.match(sha):
            fail(errors, "%s requires a 64-character lowercase sha256" % tag)
        size = m.get("size_bytes")
        if not isinstance(size, int) or size <= 0:
            fail(errors, "%s requires an exact positive size_bytes" % tag)
        url = m.get("url") or ""
        if not url.startswith("https://"):
            fail(errors, "%s must have an https:// download URL" % tag)
        if m.get("verified") != "sha256":
            fail(errors, "%s must declare verified=sha256" % tag)
        # The application catalog is the source of truth at runtime. Every pin
        # in the manifest must also exist in the compiled-in PACKS table.
        for needle in (tag, url, sha, str(size)):
            if needle and needle not in app_src:
                fail(errors, "%s pin %r is missing from src/app/osai_app.c" % (tag, needle))

    if not {"asr", "mt", "lang"} <= kinds:
        fail(errors, "catalog must offer at least one asr, mt and lang model")

    # No bundled model artifacts may remain in the release tree.
    models_dir = os.path.join(ROOT, "dist", "models")
    if os.path.isdir(models_dir) and os.listdir(models_dir):
        fail(errors, "dist/models is not empty; models must not be bundled")
    bin_models = os.path.join(ROOT, "dist", "bin", "models")
    if os.path.isdir(bin_models) and os.listdir(bin_models):
        fail(errors, "dist/bin/models is not empty; models must not ship next to the exe")

    if not errors:
        print("  ok   %d downloadable entries, all SHA-256 pinned and matched to app source" % len(models))
        print("  ok   no bundled model artifacts")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
