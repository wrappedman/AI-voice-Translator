# Model catalogue

`catalog.json` is the single place where shipped and optional models are
described. Two things read it:

* `tools/catalog/check.py` — validates the file and cross-checks it against the
  bytes that actually ship, so a stale `sha256` cannot reach a release.
* the application, when a future build wants to offer optional models. Today
  every entry has `"url": ""` and `"status": "installed"`, which means "bundled,
  nothing to download".

## Schema

| Field | Type | Meaning |
|-------|------|---------|
| `id` | string | Stable identifier. Must match the model's own id. |
| `name` | string | Shown in the Models screen. |
| `kind` | `asr` \| `mt` | Which engine consumes the file. |
| `language` | string | `fa`, `en`, or `fa;en` for bidirectional. |
| `engine` | string | `exemplar`, `trained`, or `phrasebook`. |
| `license` | string | SPDX expression. Must not be empty. |
| `homepage` | string | Where the model came from. |
| `size_bytes` | int | Exact size of the container. |
| `sha256` | string | Lowercase hex, 64 characters. |
| `url` | string | Empty for bundled models. |
| `status` | string | `installed`, `available`, or `deprecated`. |
| `min_free_gb` | string | Advisory disk requirement. |
| `notes` | string | Human-readable description. |

## Rules

1. A bundled model appears in the user's data folder on first run, so its
   `sha256` **must** equal the hash of the file the binary produces. `check.py`
   verifies this by materialising the builtin models and hashing them.
2. `id` is never reused, even after a model is retired. Bump `version` inside
   the container instead.
3. `url` may only be set for `status: available`. The application never downloads
   anything automatically; the user has to ask for it, and the download is
   verified against `sha256` before it is installed.

## Running the checker

```sh
python3 tools/catalog/check.py            # validate catalog.json
python3 tools/catalog/check.py --hash     # also hash the builtin containers
```
