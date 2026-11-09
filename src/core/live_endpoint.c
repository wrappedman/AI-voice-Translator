/* =====================================================================
 *  OpenSpeech AI — live endpointing (see live_endpoint.h for the rationale)
 * ===================================================================== */
#include "live_endpoint.h"
#include <math.h>
#include <string.h>

/* -80 dBFS: quiet enough that a silent room reads as silence, loud enough that
 * the logarithm stays a number instead of -inf. */
#define LIVE_DB_FLOOR (-80.0f)

void live_ep_defaults(live_ep_params *p) {
    if (!p) return;
    p->frame_s        = 0.02f;
    p->hangover_s     = 0.55f;   /* the "few seconds of silence" the user asked
                                  * for, shortened to where whisper behaves:
                                  * a full second of pause costs a second of
                                  * latency and answers nothing */
    p->min_speech_s   = 0.16f;   /* a click is not a sentence              */
    p->max_segment_s  = 12.0f;   /* whisper's window is 30 s; keep headroom */
    p->onset_db       = 7.0f;    /* clearly above the floor                */
    p->floor_adapt    = 0.05f;   /* ~20 frames (0.4 s) to follow the room  */
    p->floor_init_db  = -52.0f;  /* a normal quiet room before we know it  */
    p->max_frame_db   = -14.0f;  /* this loud is speech whatever the floor */
    p->cut_search_s   = 1.5f;
    p->tail_context_s = 0.20f;
    p->min_gap_flush_s= 1.20f;
}

void live_ep_init(live_ep *e, const live_ep_params *p) {
    if (!e) return;
    memset(e, 0, sizeof *e);
    if (p) e->p = *p;
    else live_ep_defaults(&e->p);
    if (e->p.frame_s <= 0) e->p.frame_s = 0.02f;
    e->floor_db = e->p.floor_init_db;
    e->recent_db = e->p.floor_init_db;
}

float live_ep_db(const float *samples, int n) {
    if (!samples || n <= 0) return LIVE_DB_FLOOR;
    double acc = 0.0;
    for (int i = 0; i < n; i++) acc += (double)samples[i] * (double)samples[i];
    float rms = (float)sqrt(acc / n);
    if (!(rms > 0.0f)) return LIVE_DB_FLOOR;
    float db = 20.0f * log10f(rms);
    return db < LIVE_DB_FLOOR ? LIVE_DB_FLOOR : db;
}

float live_ep_floor_db(const live_ep *e) { return e ? e->floor_db : 0.0f; }

/* Carries a learned noise floor into a fresh state machine, so that starting a
 * new sentence does not re-learn the room (and does not mistake the still-warm
 * tail of the previous sentence for the floor). */
void live_ep_seed_floor(live_ep *e, float floor_db) {
    if (!e) return;
    if (floor_db < -90.0f || floor_db > 0.0f) return;
    e->floor_db = floor_db;
    e->recent_db = floor_db;
    e->warm = true;
}

/* A cheap exponential smoother, so one loud frame does not move the floor and
 * one quiet frame does not hide speech. */
static float smooth(float prev, float now, float k) { return prev + (now - prev) * k; }

int live_ep_frame(live_ep *e, float frame_db) {
    if (!e) return LIVE_EP_NONE;
    const live_ep_params *p = &e->p;
    const float dt = p->frame_s;

    /* Learn the room from the quiet frames only: a floor that follows speech
     * would climb into the speech and stop hearing it.
     *
     * Cold start matters here. The first frame of a session is *not* the room if
     * the user started talking before the capture device was ready — adopting it
     * as the floor made exactly that opening word inaudible, because the speech
     * then had to be 7 dB louder than itself. So the floor starts from the
     * configured guess and only adopts the first frame when that frame looks
     * like room tone. Rise fast when the room gets quieter, slowly otherwise. */
    if (!e->warm) {
        e->warm = true;
        if (frame_db < e->floor_db + p->onset_db) e->floor_db = frame_db;
    } else if (frame_db < e->floor_db) {
        e->floor_db = smooth(e->floor_db, frame_db, 0.35f);   /* quieter: follow */
    } else if (frame_db < e->floor_db + p->onset_db * 0.6f) {
        e->floor_db = smooth(e->floor_db, frame_db, p->floor_adapt);
    }
    e->recent_db = smooth(e->recent_db, frame_db, 0.35f);

    /* Hysteresis: enter on a clear onset above the floor, stay while the frame
     * is merely above the floor. Without the two thresholds a loud noise floor
     * makes speech flicker on and off every frame. */
    const float on_db  = e->floor_db + p->onset_db;
    const float off_db = e->floor_db + p->onset_db * 0.45f;
    bool speaking = frame_db >= on_db || frame_db >= p->max_frame_db;
    if (e->in_segment && frame_db >= off_db) speaking = true;

    int event = LIVE_EP_NONE;

    if (!e->in_segment) {
        if (speaking) {
            e->speech_s += dt;
            if (e->speech_s >= p->min_speech_s) {
                e->in_segment = true;
                e->silence_s = 0;
                /* start the segment a little before the first speech frame, so
                 * the first syllable is not clipped: the pre-roll is whatever
                 * speech we saw before this point, capped at the onset window */
                e->segment_s = e->speech_s;
                e->last_speech_s = e->segment_s;
                event = LIVE_EP_SPEECH_START;
            }
        } else {
            /* decay, not reset: a door slam must not accumulate into a segment */
            e->speech_s = 0;
        }
        if (event == LIVE_EP_SPEECH_START) {
            e->events++;
            /* keep the pre-roll in the caller's buffer: it already holds it */
            e->pre_roll_s = 0;
        }
        return event;
    }

    /* Inside a segment. */
    e->segment_s += dt;
    if (speaking) {
        e->speech_s += dt;
        e->silence_s = 0;
        e->last_speech_s = e->segment_s;
    } else {
        e->silence_s += dt;
    }

    if (e->silence_s >= p->hangover_s && e->speech_s >= p->min_speech_s) {
        e->in_segment = false;
        e->speech_s = e->silence_s = 0;
        e->last_speech_s = 0;
        e->last_cut_was_forced = false;
        e->events++;
        return LIVE_EP_SEGMENT_READY;
    }
    if (e->segment_s >= p->max_segment_s) {
        /* Too long for a comfortable pass: cut at the last plausible pause and
         * let the caller keep the tail as the lead-in of the next segment. */
        e->in_segment = false;
        e->speech_s = e->silence_s = 0;
        e->last_speech_s = 0;
        e->last_cut_was_forced = true;
        e->events++;
        return LIVE_EP_SEGMENT_READY;
    }
    return LIVE_EP_NONE;
}

float live_ep_cut_point(const float *frame_db, int n_frames, int from_frame,
                        const live_ep_params *p) {
    if (!frame_db || n_frames <= 0 || !p) return 0.0f;
    if (from_frame < 0) from_frame = 0;
    if (from_frame >= n_frames) from_frame = n_frames - 1;
    int search = (int)(p->cut_search_s / (p->frame_s > 0 ? p->frame_s : 0.02f));
    int first = n_frames - search;
    if (first < from_frame) first = from_frame;
    int best = first;
    float best_db = frame_db[first];
    for (int i = first + 1; i < n_frames; i++)
        if (frame_db[i] < best_db) { best_db = frame_db[i]; best = i; }
    /* The cut goes *after* that quiet frame, i.e. all of it is recognised. */
    return (float)(best + 1) * p->frame_s;
}
