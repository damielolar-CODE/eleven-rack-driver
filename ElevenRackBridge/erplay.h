/**
 * @file erplay.h
 * @brief Rate-adaptive playback consumer for the Eleven Rack USB engine.
 *
 * Eleven Edit build (2026), on top of Matt Housley's engine. Replaces the hard
 * "re-anchor the play head and emit silence" logic in armOut with a consumer that
 * never jumps while audio is flowing:
 *
 * - The read position is FRACTIONAL (double sample time) and advances by a ratio
 *   close to 1.0 per output frame. A slow servo trims that ratio (at most ±0.4 %,
 *   i.e. below audibility) so the lag behind coreaudiod's write head sits at a
 *   target instead of drifting: the plug-in's timeline runs on mach time, the
 *   device on its own USB clock, and the two are never exactly equal. Before
 *   this, that drift walked the lag out of a fixed window every few minutes and
 *   the engine answered with a click and ~21 ms of silence.
 * - Samples are 4-point Hermite interpolated (Catmull-Rom), so a ratio of 1.002
 *   is inaudible rather than a periodic dropped/duplicated frame.
 * - A genuine underrun (the write head stalls, e.g. the source paused or a late
 *   IO cycle) fades out over a few ms and resumes with a fade in — never a
 *   step. The head keeps moving at the servo rate so, when data arrives, it is
 *   consumed from the right place without a re-prime.
 * - The lag target adapts to the host's IO burst size (coreaudiod writes one IO
 *   cycle at a time, up to 2048 frames), so a large IO buffer cannot underrun
 *   the ring between bursts.
 *
 * Header-only C so the engine and the offline simulation harness
 * (tests/erplay_sim.c) share exactly the same code.
 */
#ifndef ER_PLAY_H
#define ER_PLAY_H

#include "ERAudioRing.h"
#include <math.h>

#define ERPLAY_DEFAULT_TARGET   1024u    /**< Base lag target (frames, ~21 ms at 48 kHz). */
#define ERPLAY_MAX_RATIO_DEV    0.004    /**< Servo authority: ±0.4 % of nominal speed (inaudible). */
/* PI servo on an integrator plant (lag changes by (host - ratio) frames per
   frame): critically damped, loop time constant ~3 s at 48 kHz.
   ωn = 1/144000 per frame → Kp = 2·ωn, Ki = ωn². Errors beyond ~290 frames
   slew at the ±0.4 % authority; the integral is frozen while clamped. */
#define ERPLAY_KP               1.39e-5  /**< Proportional gain, ratio per frame of lag error. */
#define ERPLAY_KI               4.8e-11  /**< Integral gain, ratio per frame of error per frame. */
#define ERPLAY_LAG_EMA          (1.0/9600.0) /**< Lag smoothing per frame (~200 ms at 48 kHz): hides IO-cycle jitter from the servo. */
#define ERPLAY_STALL_UPDATES    4000u    /**< Updates (microframes) with no host write => source idle (0.5 s). */
#define ERPLAY_FADE_OUT_FRAMES  96.0f    /**< Underrun fade-out length (2 ms at 48 kHz). */
#define ERPLAY_FADE_IN_FRAMES   192.0f   /**< Resume fade-in length (4 ms at 48 kHz). */
#define ERPLAY_REANCHOR_EXTRA   8192u    /**< Re-prime if the lag exceeds target + this (a resumed source). */
#define ERPLAY_SEED_MAX         2048.0   /**< Largest IO burst a prime may seed the target from (coreaudiod's max IO size). */
#define ERPLAY_STOP_UPDATES     800u     /**< No host write for this many updates (100 ms) = the source stopped; stop counting underruns. */

typedef struct {
    double   pos;            /**< Fractional read position (absolute sample time). */
    double   ratio;          /**< Read step per output frame (≈1.0; >1 = consume faster). */
    double   integ;          /**< Servo integral term (dimensionless ratio offset). */
    double   lagEma;         /**< Smoothed lag = writeMax - pos (frames). */
    double   burstMax;       /**< Decaying maximum of host write bursts (frames). */
    uint64_t lastWmax;       /**< Write head at the previous update. */
    uint32_t stallUpdates;   /**< Consecutive updates with no write-head advance. */
    int      playing;        /**< 0 = idle/priming (silence), 1 = consuming. */
    int      priming;        /**< Waiting for the host to write a target's worth of FRESH audio. */
    uint64_t primeStart;     /**< Sample time the fresh audio began (first burst after idle/jump). */
    float    gain;           /**< Fade gain applied to the output (0..1). */
    float    last[ER_OUT_CH];/**< Last emitted sample per channel (held into a fade-out). */
    uint32_t baseTarget;     /**< Base lag target (frames). */
    /* diagnostics */
    uint64_t underrunFrames; /**< Output frames that had to be faded/silenced for lack of data. */
    uint32_t reanchors;      /**< Times the head was re-anchored (start/resume/stall). */
    uint32_t primes;         /**< Times playback (re)started from idle. */
} ERPlay;

/** @brief Initialise the consumer. @p baseTarget 0 = default. */
static inline void erplay_init(ERPlay *p, uint32_t baseTarget) {
    memset(p, 0, sizeof(*p));
    p->ratio = 1.0;
    p->baseTarget = baseTarget ? baseTarget : ERPLAY_DEFAULT_TARGET;
    p->burstMax = 512.0;
}

/** @brief Forget the stream (rate change / restart): next update re-primes. */
static inline void erplay_reset(ERPlay *p) {
    uint32_t t = p->baseTarget; uint64_t u = p->underrunFrames; uint32_t r = p->reanchors, pr = p->primes;
    erplay_init(p, t);
    p->underrunFrames = u; p->reanchors = r; p->primes = pr;
}

/** @brief Current lag target (frames): base plus the host's observed burst size. */
static inline double erplay_target(const ERPlay *p) { return (double)p->baseTarget + p->burstMax; }

/** @brief Catmull-Rom (4-point, 3rd-order Hermite) interpolation. */
static inline float erplay_hermite(float y0, float y1, float y2, float y3, float t) {
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/** @brief Emit @p nf frames of silence-fade (used while idle/priming/underrun). */
static inline void erplay__fade_out(ERPlay *p, float *dst, uint32_t nf) {
    for (uint32_t f = 0; f < nf; f++) {
        if (p->gain > 0.0f) { p->gain -= 1.0f / ERPLAY_FADE_OUT_FRAMES; if (p->gain < 0.0f) p->gain = 0.0f; }
        for (uint32_t c = 0; c < ER_OUT_CH; c++) dst[f * ER_OUT_CH + c] = p->last[c] * p->gain;
    }
}

/**
 * @brief Produce @p nf output frames (interleaved ::ER_OUT_CH) from the ring.
 *
 * Call once per USB microframe with the frame count the device needs. @p wmax is
 * the host's furthest written sample time (::ERRing::outWriteMax), read once per
 * isoc request by the caller.
 */
static inline void erplay_process(ERPlay *p, const ERRing *r, uint64_t wmax, float *dst, uint32_t nf) {
    /* Host activity this update: burst tracking and stall detection. */
    double burst = 0.0; int hostWrote = 0;
    if (wmax > p->lastWmax) {
        burst = (double)(wmax - p->lastWmax); hostWrote = 1;
        if (p->lastWmax != 0 && burst < 65536.0) {
            /* Track the host's IO size: jump up immediately, relax down over ~20 s of
               bursts (0.9995 per write at ~94 writes/s), so a large first write after
               StartIO does not hold the latency up, while a DAW's real 2048-frame IO
               (refreshed every cycle) keeps the cushion it needs. */
            if (burst > p->burstMax) p->burstMax = burst; else p->burstMax *= 0.9995;
            if (p->burstMax < 64.0) p->burstMax = 64.0;
        }
        p->lastWmax = wmax; p->stallUpdates = 0;
    } else if (p->lastWmax != 0) {
        if (p->stallUpdates < 0xFFFFFFFFu) p->stallUpdates++;
    }
    double target = erplay_target(p);

    /* Source went quiet for half a second: park until it comes back. */
    if (p->playing && p->stallUpdates > ERPLAY_STALL_UPDATES) { p->playing = 0; p->priming = 0; }

    if (p->playing) {
        /* A resumed source: the host's timeline kept counting while it was paused,
           so its next burst lands far ahead and the ring between holds stale audio.
           Re-prime from the fresh burst rather than play the gap. */
        double lag = (double)wmax - p->pos;
        if (lag > target + (double)ERPLAY_REANCHOR_EXTRA || lag < -(double)ER_RING_FRAMES / 4.0) {
            p->playing = 0; p->priming = 0; p->reanchors++;
        }
    }

    if (!p->playing) {
        if (!p->priming && hostWrote) {
            /* Fresh audio starts at the beginning of this burst (for a pause of any
               length, and for the very first burst). Seed the burst estimate from
               it so the lag target is right from the first prime. */
            p->priming = 1;
            p->primeStart = (burst > 0.0 && burst < 65536.0) ? wmax - (uint64_t)burst : wmax;
            /* Seed the target from this burst, capped: coreaudiod's first write after
               StartIO can be several cycles at once and would inflate the latency. */
            if (burst >= 64.0 && burst < 65536.0) p->burstMax = burst > ERPLAY_SEED_MAX ? ERPLAY_SEED_MAX : burst;
        }
        if (p->priming && wmax >= p->primeStart + (uint64_t)target) {
            p->pos = (double)p->primeStart;            /* consume the fresh audio from its start */
            p->lagEma = target; p->ratio = 1.0; p->integ = 0.0;
            p->playing = 1; p->priming = 0; p->gain = 0.0f; p->primes++;
        } else {
            erplay__fade_out(p, dst, nf);   /* idle/priming: silence, not a dropout */
            return;
        }
    }

    /* Interpolated read with a per-frame servo; underrun => fade instead of a step. */
    for (uint32_t f = 0; f < nf; f++) {
        /* Servo: smooth the lag, trim the read speed so it settles on the target. */
        double lag = (double)wmax - p->pos;
        p->lagEma += (lag - p->lagEma) * ERPLAY_LAG_EMA;
        double err = p->lagEma - target;                 /* >0: we are behind -> speed up */
        double dev = err * ERPLAY_KP + p->integ;
        int sat = 0;
        if (dev >  ERPLAY_MAX_RATIO_DEV) { dev =  ERPLAY_MAX_RATIO_DEV; sat =  1; }
        if (dev < -ERPLAY_MAX_RATIO_DEV) { dev = -ERPLAY_MAX_RATIO_DEV; sat = -1; }
        if (!(sat > 0 && err > 0) && !(sat < 0 && err < 0)) p->integ += err * ERPLAY_KI;   /* no windup while clamped */
        if (p->integ >  ERPLAY_MAX_RATIO_DEV) p->integ =  ERPLAY_MAX_RATIO_DEV;
        if (p->integ < -ERPLAY_MAX_RATIO_DEV) p->integ = -ERPLAY_MAX_RATIO_DEV;
        p->ratio = 1.0 + dev;

        double ip = floor(p->pos);
        float  t  = (float)(p->pos - ip);
        uint64_t i1 = (uint64_t)ip;
        float *d = dst + (size_t)f * ER_OUT_CH;
        if (i1 == 0 || i1 + 2 >= wmax) {
            /* Need samples the host has not written yet: hold + fade, keep moving. */
            if (p->gain > 0.0f) { p->gain -= 1.0f / ERPLAY_FADE_OUT_FRAMES; if (p->gain < 0.0f) p->gain = 0.0f; }
            for (uint32_t c = 0; c < ER_OUT_CH; c++) d[c] = p->last[c] * p->gain;
            /* A dropout is missing data while the host is still writing. Once it has
               been silent for ~100 ms the source has stopped; that is not a dropout. */
            if (p->stallUpdates <= ERPLAY_STOP_UPDATES) p->underrunFrames++;
        } else {
            if (p->gain < 1.0f) { p->gain += 1.0f / ERPLAY_FADE_IN_FRAMES; if (p->gain > 1.0f) p->gain = 1.0f; }
            const float *s0 = r->out + (size_t)((i1 - 1) & ER_RING_MASK) * ER_OUT_CH;
            const float *s1 = r->out + (size_t)((i1    ) & ER_RING_MASK) * ER_OUT_CH;
            const float *s2 = r->out + (size_t)((i1 + 1) & ER_RING_MASK) * ER_OUT_CH;
            const float *s3 = r->out + (size_t)((i1 + 2) & ER_RING_MASK) * ER_OUT_CH;
            for (uint32_t c = 0; c < ER_OUT_CH; c++) {
                float v = erplay_hermite(s0[c], s1[c], s2[c], s3[c], t) * p->gain;
                d[c] = v; p->last[c] = v;
            }
        }
        p->pos += p->ratio;
    }
}

#endif /* ER_PLAY_H */
