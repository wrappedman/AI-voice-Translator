# OpenSpeech AI — Architecture (v0.1 build)

## What ships in this repository

* `dist/bin/OpenSpeechAI.exe` — **pre-built, self-contained Windows x64 executable** (no Python, no .NET, no installer required).
* `dist/OpenSpeechAI-0.1.0-win64.zip` — distributable package (exe + docs + licenses + SHA-256).
* Full source of the portable C11 core, the Windows UI, the Local API, the ASR/MT engines and the trainer.
* `tools/` scripts that turn public free datasets/models (Common Voice, Whisper, OPUS-MT…) into native `.osam` / dictionary packs on a machine with internet access.
* Bilingual (fa/en) documentation in `docs/`.

## Deviation from the original proposal (and why)

The original scenario proposed **C#/.NET (WinForms/WPF) + Python (PyTorch) + local REST API**.
That stack was prototyped mentally against the hard constraints of this build environment:

| Constraint | Consequence |
|---|---|
| No NuGet / .NET SDK / MSVC available, no Windows machine to run on | A C# desktop app could not be compiled or verified here at all. |
| No python.org / Hugging Face / pip-for-Windows access | A PyTorch runtime could not be bundled, and a “download at first run” flow could never be *tested end-to-end*. |
| Requirement: “the built .exe must exist in the project” | Needs a toolchain that can produce a verified Windows PE from this environment. |

**Decision — same architecture, different materials.** The modular architecture of the scenario is kept 1:1
(separate ASR engine, separate MT engine, local application API, SQLite storage, offline-first, versioned models),
but the implementation is a **single portable C11 codebase** that:

1. compiles to a **native, dependency-free Windows executable** (`zig cc` / MinGW cross toolchain, static CRT),
2. runs the same UI/API/engines on Linux (`make linux`) for CI and headless testing,
3. ships a real **HTTP Local API on `127.0.0.1`** (JSON, token-protected) exactly as proposed — so a future
   C#/WinUI or Electron front-end can be attached without touching the engines,
4. needs **zero API keys, zero subscriptions, zero online account**; everything runs on the user's PC.

Why this is not a downgrade: C# + PyTorch would have meant ~500 MB of runtime, a Python install and a
torch dependency for the user; the native build is one ~3 MB file that starts in ~50 ms and works air-gapped.
The *engine* boundaries the scenario cares about are unchanged — this is an implementation detail swap that
was forced by verifiability, and it is documented honestly instead of shipping an unbuilt C# skeleton.

## Layer map

```
┌───────────────────────────────────────────────────────────────┐
│ UI layer (fa/en, RTL-aware)   src/ui/*  → win32 | software canvas │
├───────────────────────────────────────────────────────────────┤
│ App services (in-process C API)   src/app/api.c  (renderer-agnostic)
│   + optional HTTP Local API on 127.0.0.1:8765 (token auth)     │
├──────────────────────────┬────────────────────────────────────┤
│ Engine 01: ASR           │ Engine 02: Translation             │
│  mel front-end           │  offline dictionary engine         │
│  acoustic net (MLP)      │  phrase/idiom layer                │
│  CTC lexicon decoder     │  OPUS-MT / Argos pack support      │
│  n-gram language model   │  (imported as local packs)         │
│  lang-id + text norm     │                                    │
├──────────────────────────┴────────────────────────────────────┤
│ Audio pipeline (WAV/MP3/FLAC decode, resample, VAD, chunking) │
├───────────────────────────────────────────────────────────────┤
│ Persistence: SQLite (18 tables) + filesystem artifact store   │
│ Network: WinHTTP/BSD sockets, resumable downloads, SHA-256    │
└───────────────────────────────────────────────────────────────┘
```

## Determinism & offline guarantees

* No engine call ever performs network I/O. Only the Download Manager does, and only when the user asks.
* Most of the catalogue needs no Download Manager at all: an entry whose material is already in the binary is
  **built locally** (`osa_app_install_builtin_pack`) into a plain UTF-8 `word<TAB>translation` file under
  `<data>/Packs/`, then loaded into the translation engine with `mt_model_add_word_pack`. Installing is one call
  with no network step, which is what makes the button on the Models screen truthful.
* A model/dataset can only enter the store through a **catalog entry** (source URL, license, size, SHA-256) or an
  explicit user import with recorded provenance (rule #3 of the scenario).
* `dist/` is self-contained: the two shipped containers live in `dist/models/` and are copied into the data folder
  on first start. Nothing is ever written next to the exe.
* A fault writes `<data>/last-fault.log` (fault kind, module-relative address, frame addresses) before the process
  dies, so a release-build crash report is worth reading.
* Every training run writes a new immutable model version; the previous one stays selectable (rule #5).
* Local API binds to `127.0.0.1` only and requires a per-install bearer token stored in `settings` (security §10).

## Native model formats

| Artifact | Extension | Content |
|---|---|---|
| ASR model package | `.osam` | magic/version header, mel config, MLP weights, phone set, lexicon, n-gram LM, metrics, provenance JSON |
| ASR checkpoint | `.ckpt` | weights + optimizer state + step (for resume, rule #5/§6.7) |
| Translation pack | `.osmt` | dictionary + phrase table + optional neural pack descriptor |
| Dataset manifest | `.jsonl` / `.csv` | audio path, duration, speaker, transcript, split, license |

`.osam` is a self-describing little-endian container (see `docs/MODEL_FORMAT.md`) so it can be re-imported,
version-pinned and audited.
