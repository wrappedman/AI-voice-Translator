# Model formats

OpenSpeech AI ships two model containers. Both are **little-endian**, **flat**
(no compression, no pointers) and were designed so that a model is a single file
that can be copied, hashed and imported without any side data.

| Container | Extension | Produced by | Used by |
|-----------|-----------|-------------|---------|
| Acoustic / exemplar ASR | `.osam` | `asr_model_save()` | `asr_model_load()` |
| Translation phrasebook | `.osmt` | `mt_model_save()` | `mt_model_load()` |

A model is identified everywhere else by its **id**, not by its path, so a file
can move between machines freely.

---

## 1. `.osam` — speech model

The container is a fixed-size C struct written verbatim, followed by three
variable-length sections. Sizes below are in bytes; offsets assume the version 1
layout.

### Header

```c
typedef struct osam_header {
    uint32_t magic;              /* 0x4D41534F  = "OSAM" (little-endian ASCII)   */
    uint32_t version;            /* 1                                             */
    uint32_t n_tokens;           /* number of word tokens the model can emit      */
    uint32_t n_confusions;       /* number of confusion-pair entries              */
    uint32_t n_bigrams;          /* number of bigram language-model entries       */
    uint32_t layer_specs[4];     /* MLP layer widths; unused slots are 0          */
    uint32_t n_specs;            /* how many of layer_specs are meaningful        */

    char     name[128];          /* human-readable model name                     */
    char     version_s[32];      /* model version, e.g. "1.0.0"                   */
    char     language[8];        /* "fa" or "en"                                  */
    char     base_model[128];    /* id this model was derived from, or ""         */
    char     license[128];       /* SPDX expression, e.g. "GPL-3.0-or-later"      */
    char     source_url[512];    /* where the model came from, or ""              */
    char     note[256];          /* free text shown in the model card             */
    char     backend[16];        /* "exemplar" or "trained"                      */

    uint32_t feat_dim;           /* feature vector length per frame               */
    uint32_t n_exemplars;        /* number of word templates                      */
    uint32_t tmpl_floats;        /* float count in the template block             */
} osam_header;
```

### Body

Immediately after the header, in this order:

1. **Vocabulary** — `n_tokens` NUL-terminated UTF-8 strings, concatenated. A
   reader walks the block from the end of the previous token to the next `\0`.
2. **Templates** — `tmpl_floats` `float32` values, row-major. Each exemplar
   occupies a contiguous slice; `n_exemplars` describes how the slices are
   grouped by token.
3. **Confusions** — `n_confusions` pairs of `uint16_t` token indices.
4. **Bigrams** — `n_bigrams` quadruples of `(uint16 a, uint16 b, float32 logp,
   uint16 pad)`.

The current writer emits sections 1 and 2; the reader tolerates their absence
when the corresponding header count is zero, which is what forward compatibility
relies on.

### Guarantees and expectations

- **All strings are NUL-terminated inside their field.** A reader must never
  assume the field is exactly full.
- **Unknown trailing data is ignored.** That lets a newer writer append sections
  without breaking older readers, as long as `version` stays 1.
- **A different `version` is rejected**, not guessed at.
- **The file is self-contained.** Loading it never reads anything else, in
  particular never the network.

### Validation on import

`osa_app_import_model()` checks, in order:

1. the file exists, is readable, and is smaller than 512 MB;
2. the first four bytes are a known magic (`OSAM` or `OSMT`);
3. the version is one this build understands;
4. the SHA-256 is computed and stored with the model card.

If any check fails the file is left untouched and the model list is unchanged —
the app never ends up with a half-registered model.

### Integrity

There is no checksum inside the container: the SHA-256 recorded in the database
when a model is registered is the authority. Re-importing the same bytes gives
the same hash, so a catalogue entry can pin an exact model.

---

## 2. `.osmt` — translation phrasebook

```c
typedef struct mt_hdr {
    uint32_t magic;              /* 0x544D534F                                  */
    uint32_t version;            /* 1                                           */
    int32_t  n_pairs;            /* sentence pairs                              */
    int32_t  n_gloss;            /* word glosses                                */

    char     id[64];             /* model id                                    */
    char     name[128];
    char     license[128];
    char     source[512];        /* provenance, may be empty                    */
    char     backend[24];        /* written as "osmt"                        */
    char     src_tag[8];         /* "fa" or "en"                                */
    char     dst_tag[8];
} mt_hdr;
```

### Body

Each string — in **all** sections — is encoded as:

```
uint8   length       (0..250)
uint8   bytes[length] (UTF-8, not NUL-terminated)
```

Then, in order:

1. `n_pairs` × `(src string, dst string)`;
2. `n_gloss` × `(src string, dst string)`.

A length of `0` is a valid, empty string (used for "this side has no gloss").

### Matching semantics

The phrasebook is compared on **normalised** text, not on raw bytes, because
that is what makes user input match:

- Persian: Arabic yeh `U+064A` → Persian yeh `U+06CC`; Arabic kaf `U+0643` →
  `U+06A9`; ZWNJ/ZWJ and tatweel removed; runs of whitespace collapsed.
- Latin: lowercased; punctuation folded to single spaces.
- Both: leading and trailing whitespace removed.

Matching then proceeds **longest first**: a whole-sentence hit beats a multi-word
phrase, which beats a single-word gloss. Unknown tokens are passed through
unchanged rather than silently dropped, so the output shows exactly what the
engine could not translate.

---

## 3. `.ckpt` — training checkpoint

Produced during fine-tuning so an interrupted run can resume. Layout is the
`.osam` header followed by the same body, with a training-state block appended.
Checkpoints live under `.../Checkpoints/` and are never loaded at start-up; they
are an artefact, not a model.

---

## 4. Adding your own model

```sh
# 1. put the file anywhere, then
OpenSpeechAI.exe            # Models → Import model from file
```

The app copies it into `<data root>/Models/`, records its SHA-256 and size, and
registers it **inactive**. Activating is a separate, explicit step: the previous
model stays on disk, so rolling back never requires re-downloading anything.

## 5. Versioning policy

- `version` is bumped when the layout changes in a way a v1 reader cannot skip.
- New *optional* trailing sections do not bump it.
- Readers must reject unknown magics and unknown versions with a clear error
  message; they must never partially load a container.

## 6. Why not ONNX or GGUF

Both are fine formats, and both would add a runtime dependency to a program whose
main selling point is "download one file, double-click, works offline". The
containers above are ~40 lines of serialisation each, need no third-party code,
and are trivially auditable — which matters for a tool that processes private
speech.
