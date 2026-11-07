/* audio.h — decoding, resampling, VAD, chunking and feature extraction.
 * Decoders: WAV (PCM/float), MP3, FLAC, OGG/Vorbis — all in-tree, no ffmpeg. */
#ifndef OSAI_AUDIO_H
#define OSAI_AUDIO_H

#include "osai_common.h"

#define OSAI_SR 16000          /* engine sample rate */
#define OSAI_MEL_BANDS 40
#define OSAI_FRAME_LEN 400     /* 25 ms @16k */
#define OSAI_HOP 160           /* 10 ms */

typedef struct osa_audio {
    float   *samples;          /* mono, OSAI_SR */
    int64_t  n;                /* sample count */
    int      channels;         /* of the source */
    int      src_rate;
    char     path[OSAI_MAX_PATH];
} osa_audio;

void osa_audio_init(osa_audio *a);
void osa_audio_free(osa_audio *a);
bool osa_audio_load(const char *path, osa_audio *out, osa_error *err);
bool osa_audio_save_wav(const char *path, const float *samples, int64_t n, int sr);
double osa_audio_duration_s(const osa_audio *a);

/* simple energy-based VAD -> speech segments (seconds) */
typedef struct osa_segment { double start, end; float rms; } osa_segment;
int  osa_audio_vad(const osa_audio *a, osa_segment *segs, int max_segs);

/* Resampling (windowed sinc-ish, good enough for ASR features) */
void osa_audio_resample(const float *in, int64_t n_in, int sr_in, float **out, int64_t *n_out, int sr_out);

/* log-mel spectrogram: frames x bands, row-major; returns frame count */
int64_t osa_mel_spectrogram(const float *samples, int64_t n, float **out, int *n_bands);
/* peak-normalize in place */
void osa_audio_normalize(float *samples, int64_t n);
/* Estimate SNR-ish quality 0..1 (used for the quality badge) */
float osa_audio_quality(const float *samples, int64_t n);

/* synthesize a tone/noise test clip (used for demo audio without a mic) */
bool osa_audio_write_test_wav(const char *path);

#endif
