# Model formats and verification (1.4.1)

OpenSpeech AI no longer ships or accepts the old exemplar `.osam` / phrasebook
`.osmt` containers as active models. The supported artifacts are standard
upstream inference files, downloaded by the user and verified before use.

| Category | Artifact | Container | Engine |
|---|---|---|---|
| Speech (`asr`) | `ggml-{tiny,base,small}-q5_1.bin`, `ggml-medium-{q5_0,q8_0}.bin`, `ggml-large-v3-turbo-{q5_0,q8_0}.bin`, `ggml-large-v3-q5_0.bin` | whisper.cpp ggml | `whisper_bridge.cpp` |
| Translation (`mt`) | `qwen2.5-0.5b-instruct-{q4_k_m,q8_0}.gguf`, `qwen2.5-1.5b-instruct-q4_k_m.gguf`, `qwen2.5-3b-instruct-{q4_k_m,q6_k,q8_0}.gguf`, `qwen2.5-7b-instruct-q3_k_m.gguf` | GGUF (single file) | `OpenSpeechWorker` / llama.cpp |
| Language ID (`lang`) | `lid.176.{bin,ftz}` (fastText, 176 languages) and GlotLID v4 `model_v4_small.{ftz,bin}`, `model_v4.ftz`, `model.bin` (2000+ varieties) | fastText supervised | `OpenSpeechWorker` / fastText |

Multi-file GGUF splits (Qwen2.5 7B Q4_K_M and above) are deliberately not listed:
the downloader fetches one pinned file, so a split model could never be complete.

## Catalog pin

Every downloadable entry records:

- stable `id`, human name, `kind`, engine, licence and homepage;
- exact `size_bytes`;
- lowercase 64-character `sha256`;
- pinned HTTPS `url` (revision-specific where the host supports it);
- `min_ram_mb`: approximate resident memory while the model runs; shown in the
  download list («رم لازم»), used for the «سبک» badge (≤ ~1 GB) and by the
  activation memory warning;
- `status: available`, `verified: sha256`.

The runtime catalog in `src/app/osai_app.c` is the source of truth;
`tools/catalog/generate.py` regenerates the release manifest
`tools/catalog/catalog.json` from it, and `tools/catalog/check.py` cross-checks
ids, URLs, hashes, sizes and RAM figures between the two.

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
- **LID:** fastText LID 176 and GlotLID v4 return a label used for auto-detect.
  GlotLID/FLORES labels are ISO-639-3 + script (`pes_Arab`, `eng_Latn`); the
  worker normalises them to the two-letter codes Whisper and Qwen use
  (`fa`, `en`, …) in both `detect` results and the `ready.languages` list, and
  keeps the original label when no honest ISO-1 mapping exists. If the detected
  language is outside the MT set, translation fails with a readable message
  instead of silently producing garbage. On Windows the model path is quoted
  with the MSVCRT rules, parsed by the worker with `CommandLineToArgvW`, and the
  file is opened with `_wfopen` and streamed to fastText, so spaces,
  backslashes and non-ASCII (e.g. Persian) folder names work regardless of the
  ANSI code page.

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
