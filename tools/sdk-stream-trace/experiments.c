/*
 * Hardware experiments for the 2026-04-19 SDK-trace follow-up:
 *
 *   EXP1  — variable sample_interval (point 1)
 *   EXP2  — auto_stop cmd2[47]=0x28  (point 2)
 *   EXP3  — reconnect / idle stress    (point 3, Linux-side sanity check)
 *   EXP4  — cmd2 bytes 7-9 zeroed     (point 4)
 *
 * Each experiment is a subcommand. Run all:   ./experiments all
 * Or individually:                             ./experiments exp1
 *                                             ./experiments exp2 ...
 *
 * Build: make -C . experiments   (or see Makefile addition).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>

#include "picoscope2204a.h"

/* Helpers that bundle the public SDK-stream setters — each experiment
 * treats "interval_ticks" + "max_samples + auto_stop flag" as a single
 * knob, so wrap them for readability. */
static void sdk_stream_set(ps2204a_device_t *dev,
                           uint32_t interval_ticks,
                           uint64_t max_samples,
                           uint8_t  auto_stop)
{
    ps2204a_set_sdk_stream_interval_ns(dev, interval_ticks * 10);
    ps2204a_set_sdk_stream_auto_stop(dev, auto_stop ? max_samples : 0);
}

static void sdk_stream_reset(ps2204a_device_t *dev)
{
    ps2204a_set_sdk_stream_interval_ns(dev, 0);
    ps2204a_set_sdk_stream_auto_stop(dev, 0);
}

static double now_s(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static ps2204a_device_t *open_scope(void)
{
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        fprintf(stderr, "ps2204a_open failed: %d\n", st);
        return NULL;
    }
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_5V);
    return dev;
}

/* ---------- EXP1: variable interval ---------- */

static void exp1_variable_interval(void)
{
    const struct {
        uint32_t ticks;         /* 10 ns each */
        const char *label;
        double   expected_sps;
    } cases[] = {
        { 50,     "500 ns (2 MS/s)",    2e6 },
        { 100,    "1 µs  (1 MS/s)",    1e6 },
        { 200,    "2 µs  (500 kS/s)",  5e5 },
        { 500,    "5 µs  (200 kS/s)",  2e5 },
        { 1000,   "10 µs (100 kS/s)",  1e5 },
        { 2000,   "20 µs (50 kS/s)",   5e4 },
        { 5000,   "50 µs (20 kS/s)",   2e4 },
        { 10000,  "100 µs (10 kS/s)",  1e4 },
        { 20000,  "200 µs (5 kS/s)",   5e3 },
        { 50000,  "500 µs (2 kS/s)",   2e3 },
        { 100000, "1 ms   (1 kS/s)",   1e3 },
    };
    const int N = sizeof(cases) / sizeof(cases[0]);

    printf("=== EXP1 — variable sample interval ===\n");
    ps2204a_device_t *dev = open_scope();
    if (!dev) return;

    printf("%-22s %10s %12s %12s %10s %6s\n",
           "case", "ticks", "dt_ns_api", "measured_sps", "expected", "ratio");
    printf("---------------------------------------------------------------------------------\n");

    for (int i = 0; i < N; i++) {
        sdk_stream_set(dev, cases[i].ticks, /*max_samples=*/0, /*auto_stop=*/0);

        ps_status_t st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                                     0, NULL, NULL, 0);
        if (st != PS_OK) {
            printf("%-22s [START FAILED st=%d]\n", cases[i].label, st);
            continue;
        }

        int api_dt = ps2204a_get_streaming_dt_ns(dev);

        /* Run long enough to flush the async pool twice at this rate.
         * At 1 ms/sample the transfer pool is 4 × 2 KB ≈ 4 s of data, so
         * we need ≥ 5 s to get any measurement at all. */
        /* Each FPGA commit is 16 384 bytes = 8192 sample pairs. Fill time
         * at T µs/sample = 8.192 ms × T. Run long enough for ≥2 chunks. */
        double chunk_s = 8192.0 * cases[i].ticks * 10e-9;
        double run_s = chunk_s * 3.0;
        if (run_s < 2.0)  run_s = 2.0;
        if (run_s > 30.0) run_s = 30.0;
        double t0 = now_s();
        usleep((unsigned)(run_s * 1e6));
        ps_stream_stats_t stats = {0};
        ps2204a_get_streaming_stats(dev, &stats);
        double elapsed = now_s() - t0;
        (void)elapsed;
        double sps = stats.samples_per_sec > 0
                         ? stats.samples_per_sec
                         : stats.total_samples / run_s;
        ps2204a_stop_streaming(dev);
        usleep(200000);

        double ratio = sps / cases[i].expected_sps;
        printf("%-22s %10u %12d %12.0f %10.0f %6.2f\n",
               cases[i].label, cases[i].ticks, api_dt,
               sps, cases[i].expected_sps, ratio);
    }

    /* Reset to defaults. */
    sdk_stream_reset(dev);
    ps2204a_close(dev);
    printf("\n");
}

/* ---------- EXP2: auto_stop ---------- */

static volatile long g_stream_samples = 0;
static volatile int  g_stream_callbacks = 0;

static void count_cb(const float *a, const float *b, int n, void *u)
{
    (void)a; (void)b; (void)u;
    g_stream_samples += n;
    g_stream_callbacks++;
}

static void run_one_variant(ps2204a_device_t *dev, const char *label,
                            uint32_t ticks, uint64_t max_s,
                            uint8_t auto_stop, double seconds)
{
    sdk_stream_set(dev, ticks, max_s, auto_stop);
    g_stream_samples = 0;
    g_stream_callbacks = 0;

    ps_status_t st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                                  0, count_cb, NULL, 0);
    if (st != PS_OK) { printf("  %s  start failed %d\n", label, st); return; }

    double t0 = now_s();
    double stopped_at = -1;
    /* Poll the streaming flag: when auto_stop triggers client-side, the
     * driver callback clears dev->streaming and the thread exits cleanly
     * within ~200 ms. */
    while (now_s() - t0 < seconds) {
        usleep(50000);
        if (!ps2204a_is_streaming(dev)) {
            stopped_at = now_s() - t0;
            break;
        }
    }

    ps_stream_stats_t stats = {0};
    ps2204a_get_streaming_stats(dev, &stats);
    ps2204a_stop_streaming(dev);

    /* Expected capture wall-time: max_s samples × ticks × 10 ns, plus
     * ~0.3 s setup overhead (FPGA LUT + cmd chain + first chunk). */
    double target_s = (double)max_s * (double)ticks * 10e-9 + 0.3;
    printf("  %-36s  total=%-8lu sps=%-8.0f  ",
           label, (unsigned long)g_stream_samples, stats.samples_per_sec);
    if (stopped_at > 0)
        printf("STOP@%.3fs (target≈%.3fs) overshoot=%+ld\n",
               stopped_at, target_s,
               (long)g_stream_samples - (long)max_s);
    else
        printf("free-running in %.1fs\n", seconds);

    usleep(300000);
}

static void exp2_auto_stop(void)
{
    printf("=== EXP2 — auto_stop (client-side sample cap) ===\n");
    printf("Goal: confirm that stopping the streaming thread once\n"
           "      stream_samples_total >= max_samples produces a clean\n"
           "      auto_stop behaviour (matches the SDK). Matrix across\n"
           "      three sample_count values, with/without auto_stop.\n\n");

    ps2204a_device_t *dev = open_scope();
    if (!dev) return;

    /* Format: (label, ticks, max_samples, auto_stop, observe_seconds) */
    run_one_variant(dev, "A) default 10k + auto_stop=0",   100,  10000, 0, 2.0);
    run_one_variant(dev, "B) default 10k + auto_stop=1",   100,  10000, 1, 3.0);
    run_one_variant(dev, "C) 100k + auto_stop=0",          100, 100000, 0, 2.0);
    run_one_variant(dev, "D) 100k + auto_stop=1",          100, 100000, 1, 3.0);
    run_one_variant(dev, "E) 1M + auto_stop=0",            100, 1000000, 0, 2.0);
    run_one_variant(dev, "F) 1M + auto_stop=1",            100, 1000000, 1, 3.0);

    sdk_stream_reset(dev);
    ps2204a_close(dev);
    printf("\n");
}

/* ---------- EXP3: reconnect / idle stress ---------- */

static void exp3_idle_stress(void)
{
    printf("=== EXP3 — Linux reliability sweep "
           "(open/close/idle stress) ===\n");

    /* A) 10 consecutive open→stream→stop→close cycles. Should always work. */
    printf("\n  [A] 10 consecutive open/stream/close\n");
    int cycle_fails = 0;
    for (int i = 0; i < 10; i++) {
        double t0 = now_s();
        ps2204a_device_t *dev = NULL;
        ps_status_t st = ps2204a_open(&dev);
        if (st != PS_OK) { printf("    #%d open failed %d\n", i + 1, st);
                           cycle_fails++; continue; }
        ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
        ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_5V);
        st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                          0, NULL, NULL, 0);
        if (st != PS_OK) { printf("    #%d start failed %d\n", i + 1, st);
                           cycle_fails++; ps2204a_close(dev); continue; }
        usleep(400000);
        ps_stream_stats_t stats = {0};
        ps2204a_get_streaming_stats(dev, &stats);
        ps2204a_stop_streaming(dev);
        ps2204a_close(dev);
        printf("    #%2d   open+1stream+close %.2fs  samples=%lu  sps=%.0f\n",
               i + 1, now_s() - t0,
               (unsigned long)stats.total_samples, stats.samples_per_sec);
    }
    printf("  [A] failures: %d/10\n", cycle_fails);

    /* B) Open once, then 10 back-to-back streams with growing idle gap. */
    printf("\n  [B] 1 open + streams with 0s..10s idle gaps\n");
    ps2204a_device_t *dev = open_scope();
    if (!dev) return;

    int b_fails = 0;
    for (int i = 0; i < 6; i++) {
        int idle_s = i * 2;  /* 0, 2, 4, 6, 8, 10 s */
        printf("    idle=%2ds ...", idle_s); fflush(stdout);
        if (idle_s > 0) sleep(idle_s);
        double t0 = now_s();
        ps_status_t st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                                      0, NULL, NULL, 0);
        if (st != PS_OK) { printf(" FAIL start=%d\n", st); b_fails++; continue; }
        usleep(400000);
        ps_stream_stats_t stats = {0};
        ps2204a_get_streaming_stats(dev, &stats);
        ps2204a_stop_streaming(dev);
        printf(" start_ok  stream_start→stop %.2fs  samples=%lu  sps=%.0f\n",
               now_s() - t0,
               (unsigned long)stats.total_samples, stats.samples_per_sec);
    }
    printf("  [B] failures: %d/6\n", b_fails);

    ps2204a_close(dev);
    printf("\n");
}

/* ---------- EXP4: cmd2[7..9] zeroed ---------- */

static void exp4_cmd2_zero(void)
{
    printf("=== EXP4 — cmd2 bytes 7-9 stability ===\n");
    printf("Bytes 7-9 are now zeroed directly in SDK_CMD2 (baked into the\n"
           "template). This run just confirms repeated SDK streams stay\n"
           "stable with the zeroed cmd2.\n\n");

    ps2204a_device_t *dev = open_scope();
    if (!dev) return;

    const int trials = 5;
    for (int i = 0; i < trials; i++) {
        sdk_stream_reset(dev);
        ps_status_t st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                                      0, NULL, NULL, 0);
        if (st != PS_OK) { printf("    #%d start failed %d\n", i+1, st); continue; }
        usleep(500000);
        ps_stream_stats_t s = {0};
        ps2204a_get_streaming_stats(dev, &s);
        ps2204a_stop_streaming(dev);
        printf("    trial #%d  samples=%lu  sps=%.0f  blocks=%lu\n",
               i+1, (unsigned long)s.total_samples,
               s.samples_per_sec, (unsigned long)s.blocks);
        usleep(200000);
    }

    sdk_stream_reset(dev);
    ps2204a_close(dev);
    printf("\n");
}

/* ---------- driver ---------- */

int main(int argc, char **argv)
{
    const char *which = (argc > 1) ? argv[1] : "all";

    if (!strcmp(which, "exp1") || !strcmp(which, "all")) exp1_variable_interval();
    if (!strcmp(which, "exp2") || !strcmp(which, "all")) exp2_auto_stop();
    if (!strcmp(which, "exp3") || !strcmp(which, "all")) exp3_idle_stress();
    if (!strcmp(which, "exp4") || !strcmp(which, "all")) exp4_cmd2_zero();

    return 0;
}
