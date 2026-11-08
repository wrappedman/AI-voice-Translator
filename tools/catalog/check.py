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
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CATALOG = os.path.join(ROOT, "tools", "catalog", "catalog.json")
HEADLESS = os.path.join(ROOT, "build", "openspeech")

HEX64 = re.compile(r"^[0-9a-f]{64}$")
KINDS = {"asr", "mt", "lang"}
ENGINES = {"exemplar", "trained", "phrasebook", "wordpack"}
STATUSES = {"installed", "available", "manual", "deprecated"}


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
        # A hash is only meaningful for bytes we actually ship. Manual-install
        # entries carry an empty hash on purpose: an entry that claims a hash
        # nobody verified is worse than one that admits it has none.
        sha = m.get("sha256", "")
        if m.get("status") == "installed" and not HEX64.match(sha):
            fail(errors, "%s is installed but its sha256 is not 64 lowercase hex characters" % tag)
        if sha and not HEX64.match(sha):
            fail(errors, "%s sha256 is malformed" % tag)
        size = m.get("size_bytes")
        if not isinstance(size, int) or size < 0:
            fail(errors, "%s size_bytes must be a non-negative integer (0 = unknown)" % tag)
        if m.get("status") == "installed" and (not isinstance(size, int) or size <= 0):
            fail(errors, "%s is installed but has no size" % tag)
        if m.get("status") == "available" and not m.get("url"):
            fail(errors, "%s is downloadable but has no url" % tag)
        # An entry the application builds itself from material it already ships
        # is installable offline: there are no remote bytes, so there is nothing
        # to hash and nothing to fetch. `builtin://<id>` is that marker, and the
        # id must be the entry's own id so a typo cannot point at another pack.
        url = m.get("url") or ""
        builtin = url.startswith("builtin://")
        if builtin and url != "builtin://" + tag:
            fail(errors, "%s builtin url must be builtin://%s, got %s" % (tag, tag, url))
        if url and not builtin and not url.startswith("https://"):
            fail(errors, "%s url must be https:// or builtin://<id>" % tag)
        # A downloadable entry whose bytes we have not verified is a liability:
        # either publish the hash, or mark the entry as a manual install.
        if url and not builtin and not HEX64.match(sha) and m.get("status") == "available":
            fail(errors, "%s has a url but no verified sha256" % tag)
        if m.get("status") == "manual" and url:
            fail(errors, "%s is manual-install but also has a url" % tag)
        if m.get("status") == "installed" and url:
            fail(errors, "%s is installed but also has a url" % tag)
        if builtin and m.get("status") != "available":
            fail(errors, "%s is built by the application, so its status must be available" % tag)

    if not errors:
        print("  ok   %d entries structurally valid" % len(models))
        builtin = [m for m in models if (m.get("url") or "").startswith("builtin://")]
        manual = [m for m in models if m.get("status") == "manual"]
        print("  ok   %d installable offline, %d manual-install, %d installed"
              % (len(builtin), len(manual),
                 len([m for m in models if m.get("status") == "installed"])))

    if "--hash" in sys.argv:
        print("\nbuiltin containers")
        if not os.path.exists(HEADLESS):
            print("  SKIP %s not built (run `make linux`)" % HEADLESS)
            return 1 if errors else 0
        # Run against a throwaway data directory: the hashes must describe the
        # built binary's models, not whatever a previous run left in the user's
        # real profile (a stale model file there would otherwise look like drift).
        env = dict(os.environ)
        with tempfile.TemporaryDirectory(prefix="osai-catalog-") as tmp:
            env["OSAI_DATA_DIR"] = tmp
            out = subprocess.run([HEADLESS, "--models-json"], capture_output=True,
                                 text=True, env=env)
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
