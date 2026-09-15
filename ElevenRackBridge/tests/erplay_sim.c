/**
 * @file erplay_sim.c
 * @brief Offline simulation of the playback consumer (erplay.h) — no hardware.
 *
 * Models coreaudiod writing a test tone into the time-addressed ring at a host
 * clock that differs from the device clock by a chosen ppm error, in IO-cycle
 * bursts with scheduling jitter, while the device pulls 48000 frames/s in
 * 125 µs microframes through erplay_process. Reports underruns after settling,
 * the servo's final ratio, the lag, and the worst sample-to-sample step in the
 * output (a click shows up as a big step). Exit status is non-zero on failure.
 *
 * Build:  clang -O2 -o erplay_sim erplay_sim.c -I .. -lm
 * Run:    ./erplay_sim            (runs every scenario)
 */
#include "erplay.h"
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000.0

typedef struct {
    const char *name;
    double hostPpm;        /* host clock error vs device, ppm (+ = host faster) */
    uint32_t burst;        /* host IO cycle size in frames */
    double jitterMs;       /* max scheduling jitter of host writes, ms (uniform) */
    double pauseAtSec;     /* pause the source at this time (0 = never) */
    double pauseLenSec;    /* ... for this long */
    double seconds;        /* total simulated time */
} Scenario;

static double frand(void) { return rand() / (double)RAND_MAX; }

static int run(const Scenario *sc) {
    ERRing *ring = calloc(1, sizeof(ERRing));
    ERPlay play; erplay_init(&play, 0);

    const double devStep = 1.0 / 8000.0;                 /* one microframe */
    const double hostRate = RATE * (1.0 + sc->hostPpm * 1e-6);
    const double burstPeriod = sc->burst / hostRate;      /* seconds between host writes */
    double t = 0.0, nextHostWrite = 0.0, hostBase = 0.0; uint64_t burstIdx = 0;
    uint64_t hostSample = 0;                              /* host sample time written so far */
    double phase = 0.0; const double toneHz = 220.0;
    float *src = malloc(sizeof(float) * sc->burst * ER_OUT_CH);
    float out[12 * ER_OUT_CH];
    double outAccum = 0.0;

    uint64_t underAfterSettle = 0, framesAfterSettle = 0;
    double maxStep = 0.0, maxStepT = 0.0; float lastOut = 0.0f; int haveLast = 0;
    double ratioSum = 0.0, ratioMin = 9.0, ratioMax = -9.0; uint64_t ratioN = 0;
    double settle = sc->seconds * 2.0 / 3.0;              /* judge the last third of the run */
    const char *trace = getenv("ERPLAY_SIM_TRACE"); double nextTrace = 0.0;
    int paused = 0;

    while (t < sc->seconds) {
        /* Host side: write a burst when due (with jitter), unless paused. */
        if (sc->pauseAtSec > 0 && t >= sc->pauseAtSec && t < sc->pauseAtSec + sc->pauseLenSec) {
            paused = 1;
        } else if (paused) {                              /* resume: host timeline keeps counting */
            paused = 0;
            hostSample = (uint64_t)(t * hostRate);
            hostBase = t; burstIdx = 0; nextHostWrite = t;
        }
        if (!paused && t >= nextHostWrite) {
            for (uint32_t f = 0; f < sc->burst; f++) {
                float v = 0.5f * (float)sin(phase); phase += 2.0 * M_PI * toneHz / hostRate;
                for (uint32_t c = 0; c < ER_OUT_CH; c++) src[f * ER_OUT_CH + c] = v;
            }
            er_out_write_at(ring, hostSample, src, sc->burst);
            hostSample += sc->burst;
            /* Jitter around a FIXED timeline (coreaudiod schedules cycles against the
               device clock); it must not accumulate into a random walk. */
            burstIdx++;
            nextHostWrite = hostBase + burstIdx * burstPeriod + (frand() - 0.5) * 2.0 * sc->jitterMs * 1e-3;
        }
        /* Device side: one microframe. */
        outAccum += RATE / 8000.0; int nf = (int)outAccum; outAccum -= nf;
        uint64_t wmax = er_load(&ring->outWriteMax);
        uint64_t before = play.underrunFrames;
        erplay_process(&play, ring, wmax, out, (uint32_t)nf);
        if (trace && strstr(sc->name, trace) && t >= nextTrace) { nextTrace += 0.5;
            printf("   t=%5.1f lag=%7.0f lagEma=%7.0f ratio=%+6.0fppm integ=%+6.0fppm target=%.0f playing=%d\n", t,
                   (double)er_load(&ring->outWriteMax) - play.pos, play.lagEma, (play.ratio-1)*1e6, play.integ*1e6, erplay_target(&play), play.playing); }
        if (t > settle) {
            if (play.playing) { ratioSum += play.ratio; ratioN++; if (play.ratio < ratioMin) ratioMin = play.ratio; if (play.ratio > ratioMax) ratioMax = play.ratio; }
            underAfterSettle += play.underrunFrames - before;
            framesAfterSettle += nf;
            for (int f = 0; f < nf; f++) {
                float v = out[f * ER_OUT_CH];
                if (haveLast) { double d = fabs((double)v - lastOut); if (d > maxStep) { maxStep = d; maxStepT = t; } }
                lastOut = v; haveLast = 1;
            }
        }
        t += devStep;
    }
    double lag = (double)er_load(&ring->outWriteMax) - play.pos;
    /* A 220 Hz tone at 0.5 amplitude moves at most 2π·220/48000·0.5 ≈ 0.0144 per sample.
       Allow 3x for interpolation + servo speed change; a click is ≫ this. */
    double stepLimit = 0.045;
    double ratioMean = ratioN ? ratioSum / ratioN : 1.0;
    int ok = (underAfterSettle == 0) && (maxStep < stepLimit) && fabs((ratioMean - 1.0) * 1e6 - sc->hostPpm) < 150.0;
    if (sc->pauseAtSec > 0) ok = (maxStep < stepLimit) && (play.primes >= 2) && (underAfterSettle == 0) && fabs((ratioMean - 1.0) * 1e6 - sc->hostPpm) < 150.0;
    printf("%-34s %s  underrun=%llu/%llu fr  ratio mean %+.0f ppm [%+.0f..%+.0f] (host %+.0f)  lag=%.0f/%.0f fr  maxStep=%.4f @%.1fs  primes=%u reanchors=%u\n",
           sc->name, ok ? "PASS" : "FAIL", (unsigned long long)underAfterSettle, (unsigned long long)framesAfterSettle,
           (ratioMean - 1.0) * 1e6, (ratioMin - 1.0) * 1e6, (ratioMax - 1.0) * 1e6, sc->hostPpm, lag, erplay_target(&play), maxStep, maxStepT, play.primes, play.reanchors);
    free(src); free(ring);
    return ok;
}

int main(void) {
    srand(1);
    Scenario scs[] = {
        { "host +50 ppm, 512-frame IO, 1ms jit",  +50,   512, 1.0, 0, 0, 60 },
        { "host -50 ppm, 512-frame IO, 1ms jit",  -50,   512, 1.0, 0, 0, 60 },
        { "host +2000 ppm (measured 48096)",     +2000,  512, 1.0, 0, 0, 60 },
        { "host -2000 ppm",                      -2000,  512, 1.0, 0, 0, 60 },
        { "host +300 ppm, 2048-frame IO, 4ms jit", +300, 2048, 4.0, 0, 0, 60 },
        { "host +300 ppm, 64-frame IO, 0.5ms jit", +300,   64, 0.5, 0, 0, 60 },
        { "host 0 ppm, 8ms jitter storms",          0,   512, 8.0, 0, 0, 60 },
        { "pause 2s at t=10 then resume",         +100,  512, 1.0, 10, 2, 45 },
    };
    int all = 1, n = (int)(sizeof(scs) / sizeof(scs[0]));
    for (int i = 0; i < n; i++) all &= run(&scs[i]);
    printf("%s\n", all ? "ALL SCENARIOS PASS" : "SOME SCENARIOS FAILED");
    return all ? 0 : 1;
}
