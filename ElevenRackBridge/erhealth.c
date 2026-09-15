/**
 * @file erhealth.c
 * @brief Read-only ring health monitor (Eleven Edit build). Attaches to the
 *        shared ring and prints one line per second: host playback delivery rate,
 *        engine capture rate, play-head lag, servo speed, underruns, USB errors.
 *        Never touches a counter, so it cannot perturb the audio path.
 *
 * Build: clang -O2 -o erhealth erhealth.c
 * Run:   ./erhealth [seconds]
 */
#include "ERAudioRing.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    ERRing *r = er_ring_attach();
    if (!r) { printf("ring not found (engine not running?)\n"); return 1; }
    printf("ring v%u  rate %u  engineRunning=%u  streamingRequested=%u\n",
           r->version, er_load32(&r->sampleRate), er_load32(&r->engineRunning), er_load32(&r->streamingRequested));
    printf("%4s %8s %8s %8s %7s %6s %9s %8s %9s %6s\n", "t", "hostOut/s", "capIn/s", "hwFrm/s", "lag", "ppm", "underrun+", "isocErr", "reanchor", "xrun+");
    uint64_t pw = er_load(&r->outWriteMax), pi = er_load(&r->inWrite), ph = er_load(&r->hwFrames);
    uint32_t pu = er_load32(&r->playUnderrunFrames), pe = er_load32(&r->isocErrors), px = er_load32(&r->xrunCount);
    uint64_t totalUnder = 0;
    for (int t = 1; t <= secs; t++) {
        sleep(1);
        uint64_t w = er_load(&r->outWriteMax), i = er_load(&r->inWrite), h = er_load(&r->hwFrames);
        uint32_t u = er_load32(&r->playUnderrunFrames), e = er_load32(&r->isocErrors), x = er_load32(&r->xrunCount);
        totalUnder += (u - pu);
        printf("%4d %8llu %8llu %8llu %7u %+6d %9u %8u %9u %6u%s\n", t,
               (unsigned long long)(w - pw), (unsigned long long)(i - pi), (unsigned long long)(h - ph),
               er_load32(&r->playLagFrames), (int)er_load32(&r->playRatioPpm) - 1000000,
               u - pu, e, er_load32(&r->playReanchors), x - px,
               er_load32(&r->engineRunning) ? "" : "  [engine not running]");
        fflush(stdout);
        pw = w; pi = i; ph = h; pu = u; pe = e; px = x;
    }
    printf("total playback underrun frames over %d s: %llu\n", secs, (unsigned long long)totalUnder);
    return 0;
}
