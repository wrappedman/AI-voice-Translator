# OpenSpeech AI — Architecture (1.4.0)

<!-- 1.4.0: the language worker opens fastText models through _wfopen +
stdio_filebuf on Windows (non-ASCII paths), normalises ISO-639-3+script labels
(GlotLID/FLORES) to ISO-1, and activation self-heals stale model paths after
verifying size+SHA-256. PACKS carries min_ram_mb per model, surfaced in the UI
and used by the activation memory check. -->

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

`OpenSpeechWorker.exe` is also embedded in `OpenSpeechAI.exe` as the `OSAI_WORKER`
RCDATA resource. `osa_worker_path()` extracts it to `data/Cache/OpenSpeechWorker.exe`
on demand, comparing SHA-256 so a stale or truncated copy is replaced; a copy
beside the exe is used as a fallback. `CreateProcessW` gets the resolved path as
`lpApplicationName` (not just a parseable command line), the exe folder as the
working directory, and an inheritable append handle to `data/Logs/worker.log` as
stderr, so a failed model load leaves a diagnosable trace. Spawn failures report
the Windows error code.

## Worker stdio contract

The child owns exactly two inherited handles: the read end of its stdin pipe and
the write end of its stdout pipe. The parent's ends are marked non-inheritable.
Getting this wrong (clearing inheritance on "the read end" of both pipes) gave the
worker an invalid stdin: it printed `ready`, then `std::getline` failed and the
process exited, so activation looked fine and every request afterwards failed.
`tests/test_main.c` now starts the real worker with a synthetic fastText file and
performs two `detect` round trips over the same pipe.

Diagnostics: the app appends `activate kind=… model=…`, `ready`, `load failed: …`
notes to `data/Logs/worker.log`, and the worker's stderr goes to the same file. A
failed handshake quotes the worker's own error, its exit code and the log tail.

## Data layout

Everything is relative to the executable, never to the user profile:

```
<exe folder>/
  OpenSpeechAI.exe
  OpenSpeechWorker.exe        (also embedded in the exe)
  assets/...
  data/
    Models/ Downloads/ Database/ Logs/ Licenses/ Backups/
    Audio/ Exports/ Cache/ Checkpoints/ LanguagePacks/ Datasets/
    settings.ini
    last-fault.log
```

`osa_paths_init()` resolves the exe directory with `GetModuleFileNameW` /
`/proc/self/exe`, requires `<exe>/data` to be writable (a clear startup error
otherwise), and migrates a legacy `%LOCALAPPDATA%\OpenSpeechAI` (POSIX
`~/.local/share/OpenSpeechAI`) profile into it once, removing the old root only
when it is empty. `OSAI_DATA_DIR` overrides the root for tests/snapshots only.
No code path creates anything under AppData.

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
   → header names the active model per category; toast confirms the name
```

Reinstall of an already-present verified file registers it without overwriting
an active/loaded model file. **Sync** (`osa_app_sync_models`) walks the configured
models folder, matches each candidate by exact byte size and then SHA-256 against
the pinned catalogue, and registers matches inactive; anything else is counted
and named in the report, never registered. **Delete** (`osa_app_remove_model`)
unloads the engine first (an open file cannot be deleted on Windows), removes the
row and deletes the file plus leftover `.part` downloads. There is no local-file import in the UI (removed in
1.3.0): only a pinned catalogue download installs a model.

## Gating and UI state

- `osa_app_ready()` = active ASR + active MT + active LID.
- `shell_start_job()` rejects transcribe/translate when not ready, rejects
  concurrent jobs, and rejects non-restore jobs while live is running.
- `shell_select_audio()` is gated so drag/drop or tray paths cannot bypass the
  Models screen.
- The chrome subtitle lists the active ASR/MT/LID model names (or "not active");
  the status chip shows the download percentage while fetching and an
  "activating" state while engines load.
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
  isolation, shell gating, worker resolution, `<exe>/data` layout and the
  one-time profile migration (child process with a throwaway HOME).
  Optional real-weight smoke via `OSAI_TEST_*`.
- `make snapshots`: deterministic fresh empty-state renders, Persian + light.
- `make win` + `tools/verify_exe.py`: cross-build and PE/manifest/string checks,
  including that the worker payload is embedded in the app resource.
- Not covered here: native Windows mic/MF runtime, real downloaded weights
  end-to-end. Those require a Windows machine and user-supplied artifacts.
