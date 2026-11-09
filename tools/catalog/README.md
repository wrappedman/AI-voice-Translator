# Model catalogue

`catalog.json` describes the **downloadable** models offered by the application.
No weights are bundled. Two things read it:

* `tools/catalog/check.py` — validates the file and cross-checks every pin
  against the compiled-in catalog in `src/app/osai_app.c`.
* the release packager (`tools/make_dist.py`) ships it as `assets/catalog.json`
  for documentation/provenance; the runtime source of truth is the C catalog.

## Schema

| Field | Type | Meaning |
|-------|------|---------|
| `id` | string | Stable identifier. Must match the runtime catalog. |
| `name` | string | Shown in the Models screen. |
| `kind` | `asr` \| `mt` \| `lang` | Which engine consumes the file. |
| `language` | string | Capability summary (currently `multilingual`). |
| `engine` | `whisper.cpp` \| `llama.cpp` \| `fasttext` | Inference backend. |
| `license` | string | SPDX expression for the weights. |
| `homepage` | string | Upstream project/model card. |
| `size_bytes` | int | Exact artifact size. |
| `sha256` | string | Lowercase hex, 64 characters. Required. |
| `url` | string | Pinned HTTPS download URL. |
| `status` | `available` | Nothing is `installed`/bundled. |
| `min_free_gb` | string | Advisory disk requirement. |
| `notes` | string | Human-readable description. |
| `verified` | `sha256` | Integrity check performed before installation. |

## Rules

1. Every entry must have a real HTTPS URL, exact positive size and SHA-256.
   Size-only verification is not allowed.
2. `id` is never reused. Bump the pin/URL when upstream changes.
3. Installation registers the model **inactive**; the user activates it.
4. At most one active model per `kind` is enforced by the store.
5. `check.py` fails if `dist/models` or `dist/bin/models` contains artifacts.
