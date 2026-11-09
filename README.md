# OpenSpeech AI

**Offline multilingual speech recognition and translation for Windows — no bundled models.**

version 1.4.0 · Windows 10 1903+ / 11 x64 · GPL-3.0-or-later

خلاصهٔ فارسی: برنامه هیچ مدل ازپیش‌نصب‌شده‌ای همراه ندارد. در بخش «مدل‌ها» یک مدل
گفتار Whisper، یک مدل ترجمه Qwen GGUF و یک مدل تشخیص زبان (fastText یا GlotLID) را از منابع
رسمی دانلود و **فعال** می‌کنید. تا وقتی هر سه دسته مدل فعال نداشته باشند، رونویسی،
ترجمه و ترجمهٔ زنده غیرفعال می‌مانند. همهٔ داده‌ها در پوشهٔ `data` کنار فایل اجرایی
ذخیره می‌شوند و برنامه هیچ چیزی در AppData نمی‌سازد. رابط کاربری با زبان فارسی و
پوستهٔ روشن بالا می‌آید، درصد دانلود کنار دکمهٔ دانلود دیده می‌شود و نام مدل فعال
در بالای پنجره نوشته می‌شود.

```
dist/bin/OpenSpeechAI.exe          GUI executable, x86-64, static, worker embedded
dist/bin/OpenSpeechWorker.exe      isolated neural inference worker
dist/OpenSpeechAI-1.4.0-win64.zip  exe + worker + fonts/catalog/docs (no weights)
dist/checksums-1.4.0.txt           SHA-256 of exe, worker and zip
```

![Fresh-install Models screen](dist/screenshots/screen-04-models.png)

---

## What changed in 1.4.0

| Area | Behaviour |
|---|---|
| **Language-model activation on non-ASCII paths** | fastText opened its model with a narrow `std::ifstream`, so a Persian (or any non-ASCII) folder name failed *inside the worker* after the app had already verified the file. On Windows the worker now opens the file itself with the wide API (`_wfopen`) and streams it to fastText, so the path never passes through the ANSI code page. |
| **Self-healing model paths** | If the recorded path of an installed model is gone (folder moved, backup restored, profile migrated), activation searches the models folder, `data\Models` and the downloads folder for the same catalogue file, verifies size + SHA-256, repairs the database row and continues. |
| **Precise activation errors** | "file could not be opened" used to cover five different causes. Activation now distinguishes: unknown id, category mismatch, **missing file (with the expected path and a Sync hint)**, wrong size (actual vs expected bytes) and failed SHA-256 — and the failure toast wraps to up to four lines so the reason and path stay readable. |
| **RAM per model + light badges** | Every catalogue row states the approximate memory the model needs while running («رم لازم»), and models that fit in about 1 GB carry a «سبک» (light) badge. The activation memory check uses the same per-model figure. |
| **Lighter models** | Whisper Tiny Q5_1 (31 MB), Base Q5_1 (57 MB) and Small Q5_1 (181 MB) for speech; Qwen2.5 0.5B Q4/Q8 (469/644 MB) for translation — usable on 2–4 GB machines. |
| **Six language-identification models** | fastText LID 176 full/.ftz plus **GlotLID v4**: small .ftz (34 MB), small full (125 MB), full .ftz (97 MB) and full (747 MB), covering 2000+ language varieties. GlotLID's `pes_Arab`-style labels are normalised to the two-letter codes Whisper and the translator use (fa, en, ar, zh, …), in both detection results and the ready language list. |
| **Pinned catalogue tooling** | `tools/catalog/generate.py` regenerates `catalog.json` from the compiled-in table (now including `min_ram_mb`), and `check.py` validates the RAM figures against the source. New upstream URLs are pinned to immutable revisions. |

## What changed in 1.3.0

| Area | Behaviour |
|---|---|
| **Portable data folder** | Everything is created under `<exe folder>\data\` — models, downloads, database, logs, audio, cache and `settings.ini`. Nothing is written to AppData. An existing `%LOCALAPPDATA%\OpenSpeechAI` profile is moved into `data\` once, then removed if empty. |
| **Activation actually works** | `OpenSpeechWorker.exe` is embedded in `OpenSpeechAI.exe` as a resource and extracted (SHA-256 checked) to `data\Cache` on first use. The child is started with an explicit application path and working directory, inheritable pipes are set up correctly, and its stderr goes to `data\Logs\worker.log`. Failures report the real Windows error code instead of a generic "keep the worker beside the exe". |
| **Active model in the header** | The window header names the active speech, translation and language model on every screen; activating a model shows a confirmation naming it, plus a "loading/activating" state while the engines load. |
| **Download percentage** | Each downloading row shows a progress bar, the percentage next to it (bytes received when the server sends no length) and a **Cancel** button; the header chip mirrors the percentage. |
| **No local-file import** | The "add model from file" button and its file dialog were removed. Models install only from the pinned official catalogue. |
| **Bigger catalogue** | 5 Whisper variants, 5 Qwen2.5 translation quants and 2 fastText LID models (including the 0.9 MB `.ftz`), each pinned by exact size and SHA-256. |
| **Persian + light by default** | A fresh install opens in Persian with the light theme; both remain switchable in the chrome and Settings. |
| **About** | Renamed to «درباره نرم‌افزار» with the header «تشخیص گفتار و ترجمه آفلاین», plus a developer card (behnam ehsani, inbox.ehsani@gmail.com, project repository) whose email/repository links open on click. |

## What changed in 1.2.0

| Area | Behaviour |
|---|---|
| **Crash on audio selection** | Selecting a file only loads/decodes audio on a background job. The Windows M4A/AAC path was rewritten around `MFCreateSourceReaderFromURL` with balanced COM/MF lifetime and MTA; it no longer installs a custom `IStream` vtable. |
| **No bundled models** | Fresh install has zero installed/active models. The old exemplar/phrasebook containers and tiny/base Whisper entries are not shipped or auto-installed. |
| **Verified downloads** | Catalog entries are pinned by exact byte size and SHA-256. Downloads run off the UI thread, support resume/cancel, and install **inactive**. Activation is a separate explicit action. |
| **One active model per category** | Speech (`asr`), translation (`mt`) and language ID (`lang`) each have at most one active row, enforced by a SQLite partial unique index and an activation transaction. |
| **Gating** | Transcribe, translate and live require active ASR + MT + LID. File selection is also gated. The sidebar/chrome show what is missing. |
| **Model-derived languages** | Whisper language choices come from the loaded model. Translation targets come from the active translation model's capability list; auto-detect uses the active fastText LID model. |
| **Live translation** | Microphone partials use the same active models through a no-history API. Stop closes the mic and reaps the worker; shutdown joins safely. |
| **Honest training/evaluation** | Fine-tuning quantized inference artifacts is unsupported and says so. Evaluation reports zero samples / no labelled data instead of synthetic WER/CER. |
| **Long text / Unicode** | 64 KiB text buffers, strict UTF-8/surrogate JSON decoding, chunked MT with overflow/incomplete-output failure, history stores full translation/duration. |

## Downloadable models

| Category | Model | Size | Licence |
|---|---|---:|---|
| Speech | Whisper Medium Q5_0 | 514 MiB | MIT |
| Speech | Whisper Large v3 Turbo Q5_0 | 547 MiB | MIT |
| Speech | Whisper Medium Q8_0 | 785 MiB | MIT |
| Speech | Whisper Large v3 Turbo Q8_0 | 834 MiB | MIT |
| Speech | Whisper Large v3 Q5_0 | 1.01 GiB | MIT |
| Translation | Qwen2.5 1.5B Instruct Q4_K_M | 1.04 GiB | Apache-2.0 |
| Translation | Qwen2.5 3B Instruct Q4_K_M | 1.96 GiB | Apache-2.0 |
| Translation | Qwen2.5 3B Instruct Q6_K | 2.60 GiB | Apache-2.0 |
| Translation | Qwen2.5 3B Instruct Q8_0 | 3.37 GiB | Apache-2.0 |
| Translation | Qwen2.5 7B Instruct Q3_K_M | 3.55 GiB | Apache-2.0 |
| Language ID | fastText LID 176 full (`.bin`) | 125 MiB | CC-BY-SA-3.0 |
| Language ID | fastText LID 176 compact (`.ftz`) | 0.89 MiB | CC-BY-SA-3.0 |

Exact pinned URLs/hashes live in `src/app/osai_app.c` and `tools/catalog/catalog.json`.
Nothing is downloaded automatically. Inference is offline after installation.
Pick the smaller quants on a 8 GB machine; the 7B row needs ~6 GB of free RAM.

## Running

Requirements: Windows 10 version 1903 or newer / Windows 11, 64-bit. 8 GB RAM
minimum recommended for Qwen2.5 3B; several GB free disk for chosen models.
Internet is needed only when you press Download.

Extract the zip into a **writable** folder (e.g. `C:\OpenSpeechAI` or your
Desktop) and run:

```
OpenSpeechAI.exe
```

The worker is embedded in the exe and extracted to `data\Cache` when needed, so
the executable also works on its own. Keep `assets/fonts` and `assets/catalog.json`
beside it for the bundled font/catalog fallbacks. All state lives next to the exe:

```
data\Models\        downloaded, hash-verified models
data\Downloads\     partial downloads (resumable)
data\Audio\         copies of transcribed files
data\Database\      SQLite transcripts/translations/model cards
data\Logs\          fault/timing logs and worker.log (no speech text by default)
data\Cache\         extracted inference worker
data\Logs\worker.log  why a model load failed (app notes + worker stderr)
data\settings.ini   language, theme, threads, last languages
```

First run: open **Models**, download one ASR + one MT + one LID model, then
activate each. If you already have model files, set the folder in **Settings →
Models folder** and press **Sync**: recognised files appear under *Installed*.
Deleting a model from that list removes both the file and its database row. The app remains gated until all three are active, and the header
shows which model is active in each category.

If the folder is read-only (e.g. `C:\Program Files` without elevation) the app
says so at startup instead of failing later; move it somewhere writable.

## Screenshots

| | |
|---|---|
| ![Models download](dist/screenshots/screen-04b-models-download.png) | ![History](dist/screenshots/screen-03-history.png) |
| ![Settings](dist/screenshots/screen-07-settings.png) | ![English dark](dist/screenshots/screen-00b-transcribe-en-dark.png) |

Images are rendered by the application's own drawing code from a fresh empty
install (`make snapshots`, Persian + light by default). They show disabled/gated
states, not fake transcripts.

## Building

```
make bootstrap      # fetch zig cross toolchain
make linux -j2      # headless host + worker
make tests          # regression suite + fresh-install selftest
make snapshots      # regenerate dist/screenshots (add --dark for the dark set)
make win -j2        # cross-build worker, then the exe with the worker embedded
make dist           # catalog check, package zip, verify PE headers
```

Optional real-model smoke test (user-supplied official artifacts):

```
OSAI_TEST_ASR=/path/ggml-medium-q5_0.bin \
OSAI_TEST_MT=/path/qwen2.5-3b-instruct-q4_k_m.gguf \
OSAI_TEST_LID=/path/lid.176.bin \
OSAI_TEST_WAV=/path/speech.wav \
make tests
```

Without those variables the suite validates gating/catalog/store/Unicode/audio/
worker isolation, the portable `data/` layout and the one-time profile migration,
and explicitly skips real inference. `OSAI_DATA_DIR` overrides the data root for
tests only; the shipped app always uses `<exe folder>\data`.

## Validation status

- Linux: `make tests` (104 checks, including a real worker handshake and two
  `detect` round trips against a synthetic fastText fixture) and `make snapshots` pass in this environment.
- Windows: cross-build succeeds; `tools/verify_exe.py` checks PE subsystem,
  manifests, the embedded worker payload and load-bearing strings.
- **Not performed here:** native Windows microphone/Media Foundation runtime and
  real-weight download/inference end-to-end. The worker-extraction and process
  fixes are compiled and statically checked, but confirming the activation path
  on a real Windows machine still needs the downloaded models.

## Architecture

```
ui/          drawn canvas, HarfBuzz/FreeType text, immediate-mode widgets
app/         local API: gating, model lifecycle, history, downloads
worker/      isolated llama.cpp MT + fastText LID process (JSON lines)
engines/     whisper.cpp bridge (ASR), legacy helpers retained but not bundled
core/        SQLite store, audio decoders, SHA-256, HTTP, process wrapper, paths
platform/    Win32 host, Media Foundation URL reader, microphone
```

See `docs/ARCHITECTURE.md` and `docs/MODEL_FORMAT.md`.

## Developer

behnam ehsani · inbox.ehsani@gmail.com ·
<https://github.com/wrappedman/AI-voice-Translator>

## Licence

Application: GPL-3.0-or-later. Fonts: Vazirmatn (SIL OFL 1.1). Vendored engine
sources: llama.cpp (MIT), fastText (MIT), nlohmann/json (MIT). Downloaded model
weights carry their own licences, shown in the Models screen and catalog.
