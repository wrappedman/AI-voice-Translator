# OpenSpeech AI — Architecture (1.2.0)

## Product contract

1. **No bundled weights.** A fresh install has zero installed and zero active
   models. The user downloads verified artifacts and explicitly activates them.
2. **Three required categories.** Speech (`asr`), translation (`mt`) and
   language identification (`lang`). Functionality is gated until all three
   have an active model.
3. **One active model per category.** Enforced in SQLite with a partial unique
   index and a `BEGIN IMMEDIATE` activation transaction. Engines are loaded
   before the DB switch; failure preserves the previous active row.
4. **All translation flows use the active models.** File transcription
   translation, text translation and live partials share the same worker path.
   Live partials use a no-history API so repeated refreshes do not spam SQLite.
5. **Languages are capability-derived.** ASR languages come from the loaded
   Whisper model. Translation uses the supported Qwen2.5 language set; auto
   detection uses the active fastText LID model. The UI no longer hard-codes
   only Persian/English.
6. **Honest unsupported features.** Quantized inference artifacts are not
   fine-tuned in-app. Evaluation without labelled recordings reports no data,
   not synthetic scores.

## Process / layer map

```
┌────────────────────────────────────────────────────────────────────┐
│ Win32 host / headless Linux host                                   │
│   file picker → shell_select_audio (async decode, no inference)    │
├────────────────────────────────────────────────────────────────────┤
│ UI shell (src/ui)                                                  │
│   one shell_job queue: restore/load/transcribe/translate/download/ │
│   activate/import/evaluate; gating + active-model status cache     │
├────────────────────────────────────────────────────────────────────┤
│ Local app API (src/app/osai_app.c)                                 │
│   mutex-serialized engines/lifecycle/DB; cancel; chunked MT;       │
│   verified inactive install; restore/activate; history JSON        │
├──────────────────────────────┬─────────────────────────────────────┤
│ whisper.cpp bridge (in-proc) │ OpenSpeechWorker.exe (isolated)     │
│   GGUF/ggml ASR, abort/cancel │   llama.cpp GGUF MT + fastText LID  │
│   model-derived languages    │   JSON-lines stdin/stdout protocol  │
├──────────────────────────────┴─────────────────────────────────────┤
│ Core: SQLite store, audio (WAV/MP3/FLAC/OGG/M4A), SHA-256, WinHTTP/ │
│ BSD download, cross-platform process wrapper, strict UTF-8 JSON    │
└────────────────────────────────────────────────────────────────────┘
```

The worker is a separate executable so a llama.cpp/fastText failure cannot
corrupt the GUI process. The app spawns it with `CreateProcessW`/`posix_spawn`
style pipes, writes one JSON request per line and reads one JSON reply with a
deadline and cancellation. Model paths are UTF-8; both Windows executables
embed an `activeCodePage=UTF-8` manifest (Windows 10 1903+).

## Model lifecycle

```
catalog (pinned URL + size + SHA-256)
   → user presses Download
   → background fetch to .part (resume only on HTTP 206; verify on 416)
   → size + SHA-256 verification
   → copy to Models/<id>.<ext>.tmp → atomic rename
   → register installed, active=0
   → user presses Activate
   → load engine/worker first
   → DB transaction: deactivate same kind, activate this id
   → refresh model-derived languages + status cache
```

Reinstall/import of an already-present verified file registers it without
overwriting an active/loaded model file. Import accepts only files whose hash
matches a catalog entry; arbitrary `.gguf`/`.bin` files are rejected.

## Gating and UI state

- `osa_app_ready()` = active ASR + active MT + active LID.
- `shell_start_job()` rejects transcribe/translate when not ready, rejects
  concurrent jobs, and rejects non-restore jobs while live is running.
- `shell_select_audio()` is gated so drag/drop or tray paths cannot bypass the
  Models screen.
- Restore runs on init; if not ready the shell switches to Models.
- Models screen shows three active labels; About shows ASR/MT/LID rows.
- Language popups scroll arbitrarily many entries (12 visible rows) and commit
  the selected code to settings.

## Audio / Windows crash path

The reported `mfreadwrite.dll` access violation came from the old custom
`IStream`/COM lifetime path. The rewritten `m4a_mf.c`:

- dynamically loads Media Foundation by name (graceful error if absent),
- initializes MTA and balances `CoUninitialize`,
- uses `MFCreateSourceReaderFromURL` with a UTF-16 file URL instead of a
  hand-rolled `IStream` vtable,
- releases reader/types/media samples on every exit path.

File selection no longer auto-transcribes; decoding happens on the shell job
queue. Native Windows runtime validation is still required for microphone/MF.

## Storage

SQLite holds metadata only; weights/audio live on the filesystem.

- `Models(kind, active)` partial unique index `ux_model_active` guarantees one
  active row per kind.
- `osa_store_model_add()` upsert preserves the existing `active` flag.
- `osa_store_model_set_active()` uses `BEGIN IMMEDIATE`, prepared statements,
  rollback on failure, and verifies exactly one row changed.
- Transcript/translation text fields are 64 KiB; history JSON reads
  `translation`, `target_lang` and `duration_ms`.

## Downloads

`osa_http_download()` sets WinHTTP timeouts, disallows HTTPS→HTTP redirects,
resumes with `Range` only when the server answers 206, verifies existing bytes
on 416 before rename, reports correct totals, and removes `.part` on cancel.
The headless Linux build reports downloads unavailable (no TLS stack); catalog hashes are checked before installation on Windows.

## Testing / validation boundary

- `make tests`: fresh-install gate, catalog integrity, atomic activation,
  Unicode JSON, long history round trip, WAV decode, worker invalid-model
  isolation, shell gating. Optional real-weight smoke via `OSAI_TEST_*`.
- `make snapshots`: deterministic fresh empty-state renders.
- `make win` + `tools/verify_exe.py`: cross-build and PE/manifest/string checks.
- Not covered here: native Windows mic/MF runtime, real downloaded weights
  end-to-end. Those require a Windows machine and user-supplied artifacts.
