#!/usr/bin/env python3
"""Native Windows smoke test for the shipped worker.

Runs the real OpenSpeechWorker.exe with a synthetic fastText LID fixture in a
folder whose name contains spaces and Persian letters, then checks the ready
handshake and a Persian detect round trip. No model weights are downloaded.

    python tools/win_smoke.py dist/bin/OpenSpeechWorker.exe
"""
import json
import os
import subprocess
import sys
import tempfile


def main() -> int:
    if os.name != "nt":
        print("win_smoke.py must run on Windows")
        return 2
    worker = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "dist/bin/OpenSpeechWorker.exe")
    if not os.path.exists(worker):
        print("missing worker:", worker)
        return 1
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with tempfile.TemporaryDirectory(prefix="OpenSpeech smoke ") as tmp:
        folder = os.path.join(tmp, "پوشه مدل", "New folder", "data", "Models")
        os.makedirs(folder)
        fixture = os.path.join(folder, "glotlid-small.bin")
        subprocess.run([sys.executable, os.path.join(root, "tools", "make_test_lid.py"), fixture], check=True)
        req = '{"op":"detect","text":"سلام دنیا"}\n{"op":"shutdown"}\n'
        p = subprocess.run([worker, "--kind", "lang", "--model", fixture, "--threads", "1"],
                           input=req.encode("utf-8"), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=120)
        lines = [json.loads(x) for x in p.stdout.decode("utf-8").splitlines() if x.strip()]
        if p.returncode != 0 or len(lines) < 2:
            print("worker failed", p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace"))
            return 1
        ready, detect = lines[0], lines[1]
        assert ready.get("ok") and ready.get("event") == "ready" and ready.get("kind") == "lang", ready
        assert "fa" in ready.get("languages", []) and "en" in ready.get("languages", []), ready
        assert detect.get("ok") and detect.get("language") == "fa", detect
        print("ok: worker ready + Persian detect from", fixture)
    return 0


if __name__ == "__main__":
    sys.exit(main())
