/* =====================================================================
 *  OpenSpeech AI — live endpointing
 *
 *  The live screen must decide, from audio alone, where one sentence ends and
 *  the next begins, so that recognition and translation can start while the
 *  speaker is still talking instead of after the whole recording.
 *
 *  This is the little state machine that makes that decision. It is deliberately
 *  free of any audio library or UI: it consumes 20 ms energy frames and reports
 *  events, which is exactly what makes it unit-testable without a microphone.
 *
 *  Why not reuse the offline VAD (osa_audio_vad)? That one takes a complete
 *  recording and computes its threshold from the 10th percentile of the whole
 *  file. Live audio has no future to look at, so the threshold has to be learned
 *  online, from the noise floor seen so far, and it has to keep working when the
 *  room gets louder.
 *
 *  The rules, in one paragraph: a frame is speech when its energy is clearly
 *  above the running noise floor; speech starts a segment once it has lasted
 *  longer than a click; silence that lasts longer than the hangover closes the
 *  segment; a segment that grows past the window is cut at the quietest point
 *  inside it, never in the middle of a word.
 * ===================================================================== */
#ifndef OSAI_LIVE_ENDPOINT_H
#define OSAI_LIVE_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

/* Tunables, in seconds/decibels. Exposed as a struct so the tests can drive the
 * state machine with a compressed timeline instead of minutes of silence. */
typedef struct live_ep_params {
    float frame_s;            /* length of one frame            */
    float hangover_s;         /* silence that closes a segment  */
    float min_speech_s;       /* ignore clicks and door slams   */
    float max_segment_s;      /* cut a segment that grows past  */
    float onset_db;           /* speech is this far above noise */
    float floor_adapt;        /* how fast the floor forgets     */
    float floor_init_db;      /* starting floor when nothing is known */
    float max_frame_db;       /* anything louder than this is speech */
    float cut_search_s;       /* how far back a forced cut looks */
    float tail_context_s;     /* clean audio kept as lead-in of the next segment */
    float min_gap_flush_s;    /* speech immediately follows a silence this long */
} live_ep_params;

enum { LIVE_EP_NONE = 0, LIVE_EP_SPEECH_START, LIVE_EP_SEGMENT_READY };

typedef struct live_ep {
    live_ep_params p;
    /* learned noise floor and running energy, both in dBFS */
    float floor_db;
    float recent_db;
    bool  warm;
    /* current segment */
    bool     in_segment;
    float    speech_s;        /* speech inside the current segment   */
    float    silence_s;       /* trailing silence inside the segment */
    float    segment_s;       /* everything inside the segment       */
    float    last_speech_s;   /* segment time of the last speech frame */
    float    pre_roll_s;      /* speech heard just before it started */
    /* statistics for the caller and the tests */
    uint32_t events;
    /* true when the last SEGMENT_READY came from max_segment_s rather than from
     * a pause: the caller should cut at a quiet point (live_ep_cut_point) and
     * carry the tail into the next segment instead of trusting the boundary. */
    bool     last_cut_was_forced;
} live_ep;

/* Fills `p` with the production defaults (20 ms frames, 0.55 s hangover, ...). */
void live_ep_defaults(live_ep_params *p);

void live_ep_init(live_ep *e, const live_ep_params *p);

/* One 20 ms frame of mono audio, as RMS energy in dBFS (see live_ep_db). */
int live_ep_frame(live_ep *e, float frame_db);

/* Best guess at the current noise floor (dBFS), for the UI/diagnostics. */
float live_ep_floor_db(const live_ep *e);

/* Carries a learned floor into a newly initialised state (next sentence). */
void live_ep_seed_floor(live_ep *e, float floor_db);

/* The quietest point in the last `cut_search_s` of a segment that has grown
 * past `max_segment_s`: where a forced cut should go so that it does not land
 * in the middle of a word. Returns seconds from the *start* of the segment. */
float live_ep_cut_point(const float *frame_db, int n_frames, int from_frame,
                        const live_ep_params *p);

/* RMS of a frame in dBFS, with a floor so silence is a number and not -inf. */
float live_ep_db(const float *samples, int n);

#endif /* OSAI_LIVE_ENDPOINT_H */
