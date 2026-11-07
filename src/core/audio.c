#include "audio.h"
#include "osai_platform.h"
#include <math.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_libs/dr_wav.h>
#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>
#define DR_FLAC_IMPLEMENTATION
#include <dr_libs/dr_flac.h>
/* stb_vorbis ships as a single .c file; it is compiled on its own (see the
 * build files) and only its declarations are needed here. */
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis/stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

void osa_audio_init(osa_audio *a) { memset(a, 0, sizeof *a); }
void osa_audio_free(osa_audio *a) {
    free(a->samples);
    memset(a, 0, sizeof *a);
}

double osa_audio_duration_s(const osa_audio *a) {
    return a && a->n > 0 ? (double)a->n / (double)OSAI_SR : 0.0;
}

/* --- mono mixdown of interleaved float data + resample to 16k --- */
static bool finish(osa_audio *out, const float *interleaved, int64_t frames, int channels,
                   int src_rate, const char *path) {
    float *mono = (float *)malloc((size_t)frames * sizeof(float));
    if (!mono) return false;
    for (int64_t i = 0; i < frames; i++) {
        float s = 0;
        for (int ch = 0; ch < channels; ch++) s += interleaved[i * channels + ch];
        mono[i] = s / (float)channels;
    }
    out->channels = channels;
    out->src_rate = src_rate;
    if (src_rate != OSAI_SR) {
        float *res = NULL; int64_t rn = 0;
        osa_audio_resample(mono, frames, src_rate, &res, &rn, OSAI_SR);
        free(mono);
        if (!res) return false;
        out->samples = res;
        out->n = rn;
    } else {
        out->samples = mono;
        out->n = frames;
    }
    osa_strlcpy(out->path, path, sizeof out->path);
    return true;
}

bool osa_audio_load(const char *path, osa_audio *out, osa_error *err) {
    osa_audio_init(out);
    osa_err_ok(err);
    if (!osa_file_exists(path)) { osa_err_set(err, "file not found"); return false; }
    const char *ext = osa_path_ext(path);

    if (osa_strieq(ext, ".wav") || osa_strieq(ext, ".wave") || osa_strieq(ext, ".w64")) {
        drwav wav;
        if (!drwav_init_file(&wav, path, NULL)) { osa_err_set(err, "invalid WAV file"); return false; }
        int64_t frames = (int64_t)wav.totalPCMFrameCount;
        int ch = wav.channels;
        int sr = (int)wav.sampleRate;
        float *data = (float *)malloc((size_t)frames * (size_t)ch * sizeof(float));
        if (!data) { drwav_uninit(&wav); osa_err_set(err, "out of memory"); return false; }
        drwav_read_pcm_frames_f32(&wav, (drwav_uint64)frames, data);
        drwav_uninit(&wav);
        bool ok = finish(out, data, frames, ch, sr, path);
        free(data);
        if (!ok) osa_err_set(err, "decode failed");
        return ok;
    }

    if (osa_strieq(ext, ".mp3")) {
        drmp3 mp3;
        if (!drmp3_init_file(&mp3, path, NULL)) { osa_err_set(err, "invalid MP3 file"); return false; }
        drmp3_uint64 total = drmp3_get_pcm_frame_count(&mp3);
        int ch = mp3.channels;
        int sr = (int)mp3.sampleRate;
        if (!total) total = (drmp3_uint64)(mp3.totalPCMFrameCount ? mp3.totalPCMFrameCount : 0);
        if (!total) { drmp3_uninit(&mp3); osa_err_set(err, "empty MP3"); return false; }
        float *data = (float *)malloc((size_t)total * (size_t)ch * sizeof(float));
        if (!data) { drmp3_uninit(&mp3); osa_err_set(err, "out of memory"); return false; }
        drmp3_uint64 got = drmp3_read_pcm_frames_f32(&mp3, total, data);
        drmp3_uninit(&mp3);
        bool ok = finish(out, data, (int64_t)got, ch, sr, path);
        free(data);
        if (!ok) osa_err_set(err, "decode failed");
        return ok;
    }

    if (osa_strieq(ext, ".flac")) {
        unsigned ch = 0, sr = 0;
        drflac_uint64 total = 0;
        float *data = drflac_open_file_and_read_pcm_frames_f32(path, &ch, &sr, &total, NULL);
        if (!data) { osa_err_set(err, "invalid FLAC file"); return false; }
        bool ok = finish(out, data, (int64_t)total, (int)ch, (int)sr, path);
        drflac_free(data, NULL);;
        if (!ok) osa_err_set(err, "decode failed");
        return ok;
    }

    if (osa_strieq(ext, ".ogg") || osa_strieq(ext, ".oga")) {
        int ch = 0, sr = 0;
        short *pcm = NULL;
        int frames = stb_vorbis_decode_filename(path, &ch, &sr, &pcm);
        if (frames < 0 || !pcm) { osa_err_set(err, "invalid Ogg/Vorbis file"); return false; }
        float *data = (float *)malloc((size_t)frames * (size_t)ch * sizeof(float));
        if (!data) { free(pcm); osa_err_set(err, "out of memory"); return false; }
        for (int64_t i = 0; i < (int64_t)frames * ch; i++) data[i] = (float)pcm[i] / 32768.0f;
        free(pcm);
        bool ok = finish(out, data, frames, ch, sr, path);
        free(data);
        if (!ok) osa_err_set(err, "decode failed");
        return ok;
    }

    osa_err_set(err, "unsupported format '%s' (use WAV, MP3, FLAC or OGG)", ext);
    return false;
}

bool osa_audio_save_wav(const char *path, const float *samples, int64_t n, int sr) {
    drwav wav;
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 1;
    fmt.sampleRate = (drwav_uint32)sr;
    fmt.bitsPerSample = 16;
    if (!drwav_init_file_write(&wav, path, &fmt, NULL)) return false;
    int16_t *pcm = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    if (!pcm) { drwav_uninit(&wav); return false; }
    for (int64_t i = 0; i < n; i++) {
        float s = samples[i];
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        pcm[i] = (int16_t)lrintf(s * 32767.0f);
    }
    drwav_uint64 wrote = drwav_write_pcm_frames(&wav, (drwav_uint64)n, pcm);
    drwav_uninit(&wav);
    free(pcm);
    return wrote == (drwav_uint64)n;
}

/* --------------------------- resampling --------------------------- */
void osa_audio_resample(const float *in, int64_t n_in, int sr_in, float **out, int64_t *n_out, int sr_out) {
    *out = NULL; *n_out = 0;
    if (!in || n_in <= 0 || sr_in <= 0 || sr_out <= 0) return;
    double ratio = (double)sr_out / (double)sr_in;
    int64_t n = (int64_t)((double)n_in * ratio);
    float *o = (float *)malloc((size_t)n * sizeof(float));
    if (!o) return;
    /* windowed-sinc interpolation, 8 taps, hann-windowed */
    const int TAPS = 8;
    double cutoff = ratio < 1.0 ? ratio : 1.0;      /* anti-alias on downsampling */
    for (int64_t i = 0; i < n; i++) {
        double src = (double)i / ratio;
        int64_t i0 = (int64_t)floor(src);
        double acc = 0.0, wsum = 0.0;
        for (int t = -TAPS; t <= TAPS; t++) {
            int64_t idx = i0 + t;
            if (idx < 0 || idx >= n_in) continue;
            double x = src - (double)idx;
            double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * cutoff * x) / (M_PI * cutoff * x);
            double win = 0.5 + 0.5 * cos(M_PI * x / (double)(TAPS + 1));
            double w = sinc * win;
            acc += in[idx] * w;
            wsum += w;
        }
        o[i] = wsum > 1e-9 ? (float)(acc / wsum) : 0.0f;
    }
    *out = o;
    *n_out = n;
}

/* --------------------------- VAD --------------------------- */
int osa_audio_vad(const osa_audio *a, osa_segment *segs, int max_segs) {
    if (!a || !a->samples || a->n <= 0 || max_segs <= 0) return 0;
    int win = OSAI_SR / 50;                    /* 20 ms frames */
    int nframes = (int)(a->n / win);
    if (nframes < 2) return 0;
    float *rms = (float *)malloc((size_t)nframes * sizeof(float));
    if (!rms) return 0;
    float peak = 0, mean = 0;
    for (int f = 0; f < nframes; f++) {
        double acc = 0;
        const float *p = a->samples + (int64_t)f * win;
        for (int i = 0; i < win; i++) acc += (double)p[i] * p[i];
        rms[f] = (float)sqrt(acc / win);
        if (rms[f] > peak) peak = rms[f];
        mean += rms[f];
    }
    mean /= (float)nframes;
    float thr = fmaxf(mean * 1.6f, peak * 0.06f);
    if (thr < 0.002f) thr = 0.002f;

    int count = 0;
    int i = 0;
    const float pad_before = 0.12f, pad_after = 0.25f;
    const float min_len = 0.35f, max_len = 12.0f, max_sil = 0.45f;
    float frame_s = (float)win / (float)OSAI_SR;
    while (i < nframes && count < max_segs) {
        if (rms[i] <= thr) { i++; continue; }
        int start = i;
        int last_speech = i;
        int sil = 0;
        while (i < nframes && ((float)(i - start) * frame_s) < max_len) {
            if (rms[i] > thr) { last_speech = i; sil = 0; }
            else { sil++; if ((float)sil * frame_s > max_sil) break; }
            i++;
        }
        float s = (float)start * frame_s - pad_before;
        float e = (float)(last_speech + 1) * frame_s + pad_after;
        if (s < 0) s = 0;
        if (e > (float)(a->n) / OSAI_SR) e = (float)a->n / OSAI_SR;
        if (e - s >= min_len) {
            float srms = 0;
            for (int f = start; f <= last_speech && f < nframes; f++)
                if (rms[f] > srms) srms = rms[f];
            segs[count].start = s;
            segs[count].end = e;
            segs[count].rms = srms;
            count++;
        }
        i++;
    }
    free(rms);
    return count;
}

void osa_audio_normalize(float *samples, int64_t n) {
    float peak = 0;
    for (int64_t i = 0; i < n; i++) {
        float v = fabsf(samples[i]);
        if (v > peak) peak = v;
    }
    if (peak < 1e-5f) return;
    float g = 0.89f / peak;
    for (int64_t i = 0; i < n; i++) samples[i] *= g;
}

float osa_audio_quality(const float *samples, int64_t n) {
    if (!samples || n <= 0) return 0;
    double acc = 0, sig = 0;
    int64_t voiced = 0;
    for (int64_t i = 0; i < n; i++) {
        double v = samples[i];
        acc += v * v;
        if (fabs(v) > 0.01) { voiced++; sig += v * v; }
    }
    double mean = acc / (double)n;
    double vmean = voiced ? sig / (double)voiced : 0;
    if (vmean < 1e-7) return 0;
    double snr_db = 10.0 * log10(vmean / fmax(mean, 1e-9));
    float q = (float)(snr_db / 30.0);
    return osa_clampf(q, 0.0f, 1.0f);
}

/* --------------------------- log-mel --------------------------- */
static float hz_to_mel(float hz) { return 2595.0f * log10f(1.0f + hz / 700.0f); }
static float mel_to_hz(float mel) { return 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f); }

int64_t osa_mel_spectrogram(const float *samples, int64_t n, float **out, int *n_bands_out) {
    *out = NULL;
    if (n_bands_out) *n_bands_out = OSAI_MEL_BANDS;
    if (!samples || n < OSAI_FRAME_LEN) return 0;
    int nframes = (int)((n - OSAI_FRAME_LEN) / OSAI_HOP + 1);
    if (nframes <= 0) return 0;
    const int NFFT = 512;
    float *win = (float *)malloc(sizeof(float) * OSAI_FRAME_LEN);
    float *re = (float *)malloc(sizeof(float) * NFFT);
    float *im = (float *)malloc(sizeof(float) * NFFT);
    float *frame = (float *)malloc(sizeof(float) * NFFT);
    float *power = (float *)malloc(sizeof(float) * (NFFT / 2 + 1));
    float *f = (float *)calloc((size_t)nframes * OSAI_MEL_BANDS, sizeof(float));
    float *mel_fb = (float *)calloc((size_t)OSAI_MEL_BANDS * (NFFT / 2 + 1), sizeof(float));
    if (!win || !re || !im || !frame || !power || !f || !mel_fb) {
        free(win); free(re); free(im); free(frame); free(power); free(f); free(mel_fb);
        return 0;
    }
    for (int i = 0; i < OSAI_FRAME_LEN; i++) win[i] = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * i / (OSAI_FRAME_LEN - 1));

    /* mel filterbank over 80 Hz .. 7600 Hz */
    float mel_lo = hz_to_mel(80.0f), mel_hi = hz_to_mel(7600.0f);
    float centers[OSAI_MEL_BANDS + 2];
    for (int b = 0; b < OSAI_MEL_BANDS + 2; b++)
        centers[b] = mel_to_hz(mel_lo + (mel_hi - mel_lo) * (float)b / (float)(OSAI_MEL_BANDS + 1));
    float bin_hz = (float)OSAI_SR / (float)NFFT;
    for (int b = 0; b < OSAI_MEL_BANDS; b++) {
        float lo = centers[b], ce = centers[b + 1], hi = centers[b + 2];
        for (int k = 0; k <= NFFT / 2; k++) {
            float hz = bin_hz * (float)k;
            float w = 0;
            if (hz >= lo && hz <= ce && ce > lo) w = (hz - lo) / (ce - lo);
            else if (hz > ce && hz <= hi && hi > ce) w = (hi - hz) / (hi - ce);
            mel_fb[(size_t)b * (NFFT / 2 + 1) + k] = w;
        }
    }

    for (int t = 0; t < nframes; t++) {
        /* frame + pre-emphasis */
        for (int i = 0; i < OSAI_FRAME_LEN; i++) {
            float cur = samples[(int64_t)t * OSAI_HOP + i];
            float prev = (t * OSAI_HOP + i) > 0 ? samples[(int64_t)t * OSAI_HOP + i - 1] : 0.0f;
            frame[i] = (cur - 0.97f * prev) * win[i];
        }
        for (int i = OSAI_FRAME_LEN; i < NFFT; i++) frame[i] = 0;
        for (int k = 0; k < NFFT; k++) { re[k] = frame[k]; im[k] = 0; }
        /* iterative radix-2 FFT */
        for (int len = 2; len <= NFFT; len <<= 1) {
            float ang = -2.0f * (float)M_PI / (float)len;
            float wr = cosf(ang), wi = sinf(ang);
            for (int i = 0; i < NFFT; i += len) {
                float cr = 1.0f, ci = 0.0f;
                for (int k = 0; k < len / 2; k++) {
                    int a = i + k, b = i + k + len / 2;
                    float tr = re[b] * cr - im[b] * ci;
                    float ti = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr; im[a] += ti;
                    float ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = ncr;
                }
            }
        }
        for (int k = 0; k <= NFFT / 2; k++) power[k] = re[k] * re[k] + im[k] * im[k];
        for (int b = 0; b < OSAI_MEL_BANDS; b++) {
            float acc = 0;
            for (int k = 0; k <= NFFT / 2; k++) acc += power[k] * mel_fb[(size_t)b * (NFFT / 2 + 1) + k];
            f[(size_t)t * OSAI_MEL_BANDS + b] = logf(acc + 1e-8f);
        }
    }
    free(win); free(re); free(im); free(frame); free(power); free(mel_fb);
    *out = f;
    return nframes;
}

bool osa_audio_write_test_wav(const char *path) {
    /* 3 s of a formant-ish synthesized "speech-like" signal for demos/tests */
    const int sr = OSAI_SR;
    int n = sr * 3;
    float *s = (float *)calloc((size_t)n, sizeof(float));
    if (!s) return false;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)sr;
        float env = 0.5f + 0.5f * sinf(2.0f * (float)M_PI * 2.2f * t);
        float f0 = 118.0f + 12.0f * sinf(2.0f * (float)M_PI * 0.7f * t);
        float v = 0;
        for (int h = 1; h <= 12; h++) v += sinf(2.0f * (float)M_PI * f0 * (float)h * t) / (float)h;
        v *= env * 0.25f;
        s[i] = v;
    }
    bool ok = osa_audio_save_wav(path, s, n, sr);
    free(s);
    return ok;
}
