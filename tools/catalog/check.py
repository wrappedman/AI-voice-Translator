#!/usr/bin/env python3
"""Validate the shipped model catalogue.

Two levels:

    check.py          structural validation of catalog.json
    check.py --hash   additionally materialise the builtin models through the
                      application API and confirm the recorded hashes match

The second mode requires a built `build/tests`-style binary; it shells out to
the headless host so the hashes come from the same code path the app uses,
rather than being copied around by hand.
"""
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOG = os.path.join(ROOT, "tools", "catalog", "catalog.json")
HEADLESS = os.path.join(ROOT, "build", "openspeech")

HEX64 = re.compile(r"^[0-9a-f]{64}$")
KINDS = {"asr", "mt"}
ENGINES = {"exemplar", "trained", "phrasebook"}
STATUSES = {"installed", "available", "deprecated"}


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

    print("catalog.json")
    if doc.get("schema") != 1:
        fail(errors, "schema must be 1")
    models = doc.get("models")
    if not isinstance(models, list) or not models:
        fail(errors, "models must be a non-empty array")
        models = []

    seen = set()
    for i, m in enumerate(models):
        tag = m.get("id", "#%d" % i)
        if not m.get("id"):
            fail(errors, "entry %d has no id" % i)
            continue
        if tag in seen:
            fail(errors, "duplicate id %s" % tag)
        seen.add(tag)
        if not m.get("name"):
            fail(errors, "%s has no name" % tag)
        if m.get("kind") not in KINDS:
            fail(errors, "%s kind must be one of %s" % (tag, sorted(KINDS)))
        if m.get("engine") not in ENGINES:
            fail(errors, "%s engine must be one of %s" % (tag, sorted(ENGINES)))
        if m.get("status") not in STATUSES:
            fail(errors, "%s status must be one of %s" % (tag, sorted(STATUSES)))
        if not m.get("license"):
            fail(errors, "%s has no license" % tag)
        sha = m.get("sha256", "")
        if not HEX64.match(sha):
            fail(errors, "%s sha256 is not 64 lowercase hex characters" % tag)
        size = m.get("size_bytes")
        if not isinstance(size, int) or size <= 0:
            fail(errors, "%s size_bytes must be a positive integer" % tag)
        if m.get("status") == "available" and not m.get("url"):
            fail(errors, "%s is downloadable but has no url" % tag)
        if m.get("status") == "installed" and m.get("url"):
            fail(errors, "%s is installed but also has a url" % tag)

    if not errors:
        print("  ok   %d entries structurally valid" % len(models))

    if "--hash" in sys.argv:
        print("\nbuiltin containers")
        if not os.path.exists(HEADLESS):
            print("  SKIP %s not built (run `make linux`)" % HEADLESS)
            return 1 if errors else 0
        out = subprocess.run([HEADLESS, "--models-json"], capture_output=True, text=True)
        if out.returncode != 0:
            fail(errors, "headless host failed: %s" % (out.stderr.strip() or out.returncode))
        else:
            try:
                live = json.loads(out.stdout)
            except json.JSONDecodeError:
                fail(errors, "host returned invalid JSON")
                live = []
            by_id = {m["id"]: m for m in live}
            for m in models:
                if m.get("status") != "installed":
                    continue
                got = by_id.get(m["id"])
                if not got:
                    fail(errors, "%s is in the catalogue but not registered" % m["id"])
                    continue
                if got.get("sha256") != m["sha256"]:
                    fail(errors, "%s sha256 drift: catalog %s, build %s"
                         % (m["id"], m["sha256"][:16], got.get("sha256", "?")[:16]))
                elif got.get("bytes") != m["size_bytes"]:
                    fail(errors, "%s size drift: catalog %d, build %d"
                         % (m["id"], m["size_bytes"], got.get("bytes", -1)))
                else:
                    print("  ok   %s hash and size match the built binary" % m["id"])

    print()
    if errors:
        print("%d problem(s)" % len(errors))
        return 1
    print("catalog is consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
