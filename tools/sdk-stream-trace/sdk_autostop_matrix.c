/*
 * sdk_autostop_matrix — parametric SDK capture focused on decoding how
 * the SDK packs (max_samples, interval) into cmd1 when auto_stop=1.
 *
 * For each phase we use N×flash_led as in-band marker (phase index = N).
 * We probe 8 combinations:
 *
 *   Phase 1 : as=0 ms=100000 iv=1us         — baseline (no autostop)
 *   Phase 2 : as=1 ms=10000  iv=1us         (10k × 1000ns = 10 ms total)
 *   Phase 3 : as=1 ms=100000 iv=1us         (100k × 1000ns = 100 ms, same as legacy phase22)
 *   Phase 4 : as=1 ms=1000000 iv=1us        (1M × 1000ns = 1 s)
 *   Phase 5 : as=1 ms=100000 iv=500ns       (100k × 500ns = 50 ms)
 *   Phase 6 : as=1 ms=100000 iv=2us         (100k × 2000ns = 200 ms)
 *   Phase 7 : as=1 ms=100000 iv=10us        (100k × 10000ns = 1 s)
 *   Phase 8 : as=1 ms=10000  iv=10us        (10k × 10000ns = 100 ms)
 *   Phase 9 : as=1 ms=50000  iv=1us         (50k × 1000ns = 50 ms, sanity bis)
 *   Phase 10: as=1 ms=200000 iv=5us         (200k × 5000ns = 1 s, sanity bis)
 *
 * Build:
 *   gcc sdk_autostop_matrix.c -o sdk_autostop_matrix \
 *       -I/opt/picoscope/include/libps2000 \
 *       -L/opt/picoscope/lib -lps2000 \
 *       -Wl,-rpath,/opt/picoscope/lib
 *
 * Run under LD_PRELOAD so each USB command is logged:
 *   LD_PRELOAD=$PWD/../firmware-extractor/libps_intercept.so \
 *       ./sdk_autostop_matrix 2>&1 | tee sdk_autostop.stdout
 *   mv usb_trace.log sdk_autostop_trace.log
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ps2000.h"

static volatile long g_total_samples = 0;

static void PREF4 stream_cb(int16_t **buffers,
                            int16_t  overflow,
                            uint32_t triggeredAt,
                            int16_t  triggered,
                            int16_t  auto_stop,
                            uint32_t n_values)
{
    (void)buffers; (void)overflow; (void)triggeredAt;
    (void)triggered; (void)auto_stop;
    g_total_samples += (long)n_values;
}

static void mark_phase(int16_t h, int n, const char *label)
{
    printf("\n===== PHASE %02d : %s =====\n", n, label);
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        ps2000_flash_led(h);
        usleep(20000);
    }
    usleep(250000);
}

static void run_stream(int16_t h,
                       uint32_t interval,
                       PS2000_TIME_UNITS units,
                       uint32_t max_samples,
                       int16_t  auto_stop,
                       double   seconds)
{
    g_total_samples = 0;
    if (!ps2000_run_streaming_ns(h, interval, units, max_samples,
                                 auto_stop, 1 /*agg*/, 50000 /*ovbuf*/)) {
        fprintf(stderr, "  run_streaming_ns(ival=%u u=%d ms=%u as=%d) FAILED\n",
                interval, units, max_samples, auto_stop);
        return;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (dt >= seconds) break;
        ps2000_get_streaming_last_values(h, stream_cb);
        usleep(20000);
    }
    ps2000_stop(h);
    printf("  samples=%ld over %.1fs\n", g_total_samples, seconds);
}

int main(void)
{
    printf("[autostop] opening unit (cold-plug)...\n");
    int16_t h = ps2000_open_unit();
    if (h <= 0) { fprintf(stderr, "ps2000_open_unit failed: %d\n", h); return 1; }
    printf("[autostop] handle=%d\n", h);

    int8_t info[64];
    ps2000_get_unit_info(h, info, sizeof(info), 4);
    printf("[autostop] serial=%s\n", info);

    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);

    /* Phase 1 — baseline as=0 */
    mark_phase(h, 1, "BASELINE as=0 ms=100000 iv=1us");
    run_stream(h, 1, PS2000_US, 100000, 0, 1.5);

    /* Phase 2 — as=1 ms=10k iv=1us */
    mark_phase(h, 2, "as=1 ms=10000 iv=1us (10ms total)");
    run_stream(h, 1, PS2000_US, 10000, 1, 1.5);

    /* Phase 3 — as=1 ms=100k iv=1us (reference: legacy phase 22) */
    mark_phase(h, 3, "as=1 ms=100000 iv=1us (100ms total)");
    run_stream(h, 1, PS2000_US, 100000, 1, 1.5);

    /* Phase 4 — as=1 ms=1M iv=1us */
    mark_phase(h, 4, "as=1 ms=1000000 iv=1us (1s total)");
    run_stream(h, 1, PS2000_US, 1000000, 1, 2.0);

    /* Phase 5 — as=1 ms=100k iv=500ns */
    mark_phase(h, 5, "as=1 ms=100000 iv=500ns (50ms total)");
    run_stream(h, 500, PS2000_NS, 100000, 1, 1.5);

    /* Phase 6 — as=1 ms=100k iv=2us */
    mark_phase(h, 6, "as=1 ms=100000 iv=2us (200ms total)");
    run_stream(h, 2, PS2000_US, 100000, 1, 1.5);

    /* Phase 7 — as=1 ms=100k iv=10us */
    mark_phase(h, 7, "as=1 ms=100000 iv=10us (1s total)");
    run_stream(h, 10, PS2000_US, 100000, 1, 2.0);

    /* Phase 8 — as=1 ms=10k iv=10us */
    mark_phase(h, 8, "as=1 ms=10000 iv=10us (100ms total)");
    run_stream(h, 10, PS2000_US, 10000, 1, 1.5);

    /* Phase 9 — as=1 ms=50k iv=1us */
    mark_phase(h, 9, "as=1 ms=50000 iv=1us (50ms total)");
    run_stream(h, 1, PS2000_US, 50000, 1, 1.5);

    /* Phase 10 — as=1 ms=200k iv=5us */
    mark_phase(h, 10, "as=1 ms=200000 iv=5us (1s total)");
    run_stream(h, 5, PS2000_US, 200000, 1, 2.0);

    /* Phase 11 — END marker */
    mark_phase(h, 11, "END");

    ps2000_close_unit(h);
    printf("[autostop] done.\n");
    return 0;
}
