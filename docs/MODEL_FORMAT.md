# Model formats and verification (1.3.1)

OpenSpeech AI no longer ships or accepts the old exemplar `.osam` / phrasebook
`.osmt` containers as active models. The supported artifacts are standard
upstream inference files, downloaded by the user and verified before use.

| Category | Artifact | Container | Engine |
|---|---|---|---|
| Speech (`asr`) | `ggml-medium-q5_0.bin`, `ggml-medium-q8_0.bin`, `ggml-large-v3-turbo-q5_0.bin`, `ggml-large-v3-turbo-q8_0.bin`, `ggml-large-v3-q5_0.bin` | whisper.cpp ggml | `whisper_bridge.cpp` |
| Translation (`mt`) | `qwen2.5-1.5b-instruct-q4_k_m.gguf`, `qwen2.5-3b-instruct-{q4_k_m,q6_k,q8_0}.gguf`, `qwen2.5-7b-instruct-q3_k_m.gguf` | GGUF (single file) | `OpenSpeechWorker` / llama.cpp |
| Language ID (`lang`) | `lid.176.bin` (125 MiB) and `lid.176.ftz` (0.89 MiB, quantized) | fastText supervised | `OpenSpeechWorker` / fastText |

Multi-file GGUF splits (Qwen2.5 7B Q4_K_M and above) are deliberately not listed:
the downloader fetches one pinned file, so a split model could never be complete.

## Catalog pin

Every downloadable entry records:

- stable `id`, human name, `kind`, engine, licence and homepage;
- exact `size_bytes`;
- lowercase 64-character `sha256`;
- pinned HTTPS `url` (revision-specific where the host supports it);
- `status: available`, `verified: sha256`.

The runtime catalog in `src/app/osai_app.c` and the release manifest
`tools/catalog/catalog.json` are cross-checked by `tools/catalog/check.py`.

## Install rules

1. Download to `<Downloads>/<id>.<ext>.part` with resume/cancel support.
2. Verify byte size and SHA-256 against the catalog pin.
3. Copy to `<Models>/<id>.<ext>.tmp`, verify again, atomically rename.
4. Register the model **inactive**. Activation is a separate user action.
5. **There is no local-file import.** The UI button, file dialog and job kind were
   removed in 1.3.0: only a catalogue download can install a model, so an
   arbitrary GGUF/ggml/fastText file can never become an engine. The app-layer
   helper `osa_app_install_pack_file` still exists for verified reinstall of a
   file whose hash matches a catalogue pin, but no UI path reaches it.
6. Reinstalling an already-present verified file registers it without
   overwriting an active/loaded model file.
7. **Sync** applies the same rule to files the user already has: the configured
   models folder is scanned, candidates are matched by exact size and SHA-256, and
   only matches are registered (inactive). Unknown files are reported by name.
8. **Delete** removes the database row and the file itself, after unloading the
   engine. It is the only destructive model operation and needs two clicks.

## Activation

Activation resolves the worker (`osa_worker_path`): on Windows it extracts the
embedded `OSAI_WORKER` resource into `data/Cache/OpenSpeechWorker.exe` after a
SHA-256 check, falling back to a copy beside the exe. The child is spawned with an
explicit application path, `data`-relative working directory and its stderr
appended to `data/Logs/worker.log`. Activation then loads the engine/worker and
switches the SQLite active row in a transaction. A partial unique index guarantees at most one active model
per `kind`. Failure leaves the previous active model intact.

## Language capabilities

- **ASR:** language list is read from the loaded Whisper model
  (`whisper_full_lang_max_id` / language strings). The UI popups and settings
  use that list; there is no fixed fa/en rejection.
- **MT:** each translation pack carries its own language list (the Qwen2.5 set is
  `en fa ar zh fr es pt de it ru ja ko vi th id tr hi he nl pl uk`), recorded on
  the installed model row and read back by the UI. This is an
  application capability list, not a claim that GGUF metadata enumerates
  translation directions. Unsupported targets are rejected before inference.
- **LID:** fastText LID 176 returns a label used for auto-detect. If the
  detected language is outside the MT set, translation fails with a readable
  message instead of silently producing garbage.

## Text limits and failure behaviour

- UI/app text buffers: 64 KiB (`OSAI_MAX_TEXT`).
- MT input is chunked at ≤2400 UTF-8 bytes per request.
- Worker reply overflow, timeout, cancellation or incomplete output fails the
  operation; partial translations are not saved as complete history rows.
- History stores `translation`, `target_lang` and `duration_ms` alongside the
  transcript.

## Legacy containers

`.osam` / `.osmt` code remains in the tree for historical reference and some
helper engines, but they are not bundled, not auto-installed, and not accepted
by any install path. Training/evaluation no longer fabricate exemplar or
phrasebook scores.
