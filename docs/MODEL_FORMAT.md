# Model formats and verification (1.2.0)

OpenSpeech AI no longer ships or accepts the old exemplar `.osam` / phrasebook
`.osmt` containers as active models. The supported artifacts are standard
upstream inference files, downloaded by the user and verified before use.

| Category | Artifact | Container | Engine |
|---|---|---|---|
| Speech (`asr`) | `ggml-medium-q5_0.bin`, `ggml-large-v3-turbo-q5_0.bin` | whisper.cpp ggml | `whisper_bridge.cpp` |
| Translation (`mt`) | `qwen2.5-3b-instruct-q4_k_m.gguf` | GGUF | `OpenSpeechWorker` / llama.cpp |
| Language ID (`lang`) | `lid.176.bin` (full, **not** `.ftz`) | fastText supervised | `OpenSpeechWorker` / fastText |

## Catalog pin

Every downloadable entry records:

- stable `id`, human name, `kind`, engine, licence and homepage;
- exact `size_bytes`;
- lowercase 64-character `sha256`;
- pinned HTTPS `url` (revision-specific where the host supports it);
- `status: available`, `verified: sha256`.

The runtime catalog in `src/app/osai_app.c` and the release manifest
`tools/catalog/catalog.json` are cross-checked by `tools/catalog/check.py`.

## Install / import rules

1. Download to `<Downloads>/<id>.<ext>.part` with resume/cancel support.
2. Verify byte size and SHA-256 against the catalog pin.
3. Copy to `<Models>/<id>.<ext>.tmp`, verify again, atomically rename.
4. Register the model **inactive**. Activation is a separate user action.
5. Local import (`shell_import_model` / `osa_app_install_pack_file`) hashes the
   chosen file and accepts it **only** if the hash matches a catalog entry.
   Arbitrary GGUF/ggml/fastText files are rejected.
6. Reinstalling an already-present verified file registers it without
   overwriting an active/loaded model file.

## Activation

Activation loads the engine/worker first, then switches the SQLite active row
in a transaction. A partial unique index guarantees at most one active model
per `kind`. Failure leaves the previous active model intact.

## Language capabilities

- **ASR:** language list is read from the loaded Whisper model
  (`whisper_full_lang_max_id` / language strings). The UI popups and settings
  use that list; there is no fixed fa/en rejection.
- **MT:** the worker uses a conservative supported-language set for Qwen2.5
  (`en fa ar zh fr es pt de it ru ja ko vi th id tr hi he nl pl uk`). This is an
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
by the verified import path. Training/evaluation no longer fabricate exemplar
or phrasebook scores.
