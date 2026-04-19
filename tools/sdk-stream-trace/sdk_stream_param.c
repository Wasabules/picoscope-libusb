/*
 * Experiment 2 — parametric SDK capture on a single cold-plug session.
 *
 * Opens the unit ONCE, then runs many configurations back-to-back, varying
 * one parameter at a time, so the LD_PRELOAD trace contains a sequence of
 * clearly-delimited "phases" we can diff to identify what each byte means.
 *
 * Phase boundaries are marked in-band: between each phase we call
 * ps2000_flash_led() N times where N = phase index. Each flash issues a
 * unique USB command easy to spot in the trace; a run of N flashes
 * announces phase N.
 *
 * Build:
 *   gcc sdk_stream_param.c -o sdk_stream_param \
 *       -I/opt/picoscope/include/libps2000 \
 *       -L/opt/picoscope/lib -lps2000 \
 *       -Wl,-rpath,/opt/picoscope/lib
 *
 * Run (fresh cold-plug, under the interceptor):
 *   LD_PRELOAD=$PWD/../firmware-extractor/libps_intercept.so \
 *       ./sdk_stream_param 2>&1 | tee sdk_param.stdout
 *   mv usb_trace.log sdk_param_trace.log
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

/* In-band marker: N consecutive ps2000_flash_led calls = phase N. */
static void mark_phase(int16_t h, int n, const char *label)
{
    printf("\n===== PHASE %02d : %s =====\n", n, label);
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        ps2000_flash_led(h);
        usleep(20000);
    }
    /* short silence after markers so they're easy to find visually */
    usleep(200000);
}

/* Run one streaming capture with the given config. Assumes channels already set. */
static void run_stream(int16_t h,
                       uint32_t interval,
                       PS2000_TIME_UNITS units,
                       uint32_t max_samples,
                       int16_t  auto_stop,
                       int16_t  aggregate,
                       uint32_t overview_buf,
                       double   seconds)
{
    g_total_samples = 0;
    if (!ps2000_run_streaming_ns(h, interval, units, max_samples,
                                 auto_stop, aggregate, overview_buf)) {
        fprintf(stderr, "  run_streaming_ns(ival=%u u=%d ms=%u as=%d ag=%d ob=%u) FAILED\n",
                interval, units, max_samples, auto_stop, aggregate, overview_buf);
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

static const char *range_name(PS2000_RANGE r)
{
    switch (r) {
    case PS2000_50MV:  return "50mV";
    case PS2000_100MV: return "100mV";
    case PS2000_200MV: return "200mV";
    case PS2000_500MV: return "500mV";
    case PS2000_1V:    return "1V";
    case PS2000_2V:    return "2V";
    case PS2000_5V:    return "5V";
    case PS2000_10V:   return "10V";
    case PS2000_20V:   return "20V";
    default:           return "?";
    }
}

int main(void)
{
    printf("[param] opening unit (cold-plug)...\n");
    int16_t h = ps2000_open_unit();
    if (h <= 0) { fprintf(stderr, "ps2000_open_unit failed: %d\n", h); return 1; }
    printf("[param] handle=%d\n", h);

    int8_t info[64];
    ps2000_get_unit_info(h, info, sizeof(info), 4);
    printf("[param] serial=%s\n", info);

    /* ================================================================
     * Phase 1 — BASELINE: CH A + B, DC, ±5V, 1us/sample (1 MS/s)
     * ================================================================ */
    mark_phase(h, 1, "baseline A+B DC 5V 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.5);

    /* ================================================================
     * Phase 2 — CH A only (B disabled)
     *   Isolates which bytes belong to channel B.
     * ================================================================ */
    mark_phase(h, 2, "A only, B disabled, DC 5V 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 0, 1, PS2000_5V);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.5);

    /* ================================================================
     * Phase 3 — CH B only (A disabled)
     *   Isolates which bytes belong to channel A.
     * ================================================================ */
    mark_phase(h, 3, "B only, A disabled, DC 5V 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 0, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.5);

    /* ================================================================
     * Phase 4..9 — sweep CH A range (B kept at 5V DC)
     *   Identifies where the CH A range/PGA bytes live.
     * ================================================================ */
    PS2000_RANGE ranges[] = {
        PS2000_100MV, PS2000_200MV, PS2000_500MV,
        PS2000_1V,    PS2000_2V,    PS2000_20V
    };
    for (unsigned i = 0; i < sizeof(ranges)/sizeof(ranges[0]); i++) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "A range=%s B=5V DC 1us",
                 range_name(ranges[i]));
        mark_phase(h, 4 + (int)i, lbl);
        ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, ranges[i]);
        ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);
        run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.2);
    }

    /* ================================================================
     * Phase 10 — CH B range change only (A back to 5V).
     *   Second witness for the B range/PGA bytes.
     * ================================================================ */
    mark_phase(h, 10, "A=5V B=200mV DC 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_200MV);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.2);

    /* ================================================================
     * Phase 11 — AC coupling on CH A
     *   Identifies the coupling byte / DC-vs-AC bit.
     * ================================================================ */
    mark_phase(h, 11, "A AC 5V, B DC 5V, 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 0 /* AC */, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.2);

    /* ================================================================
     * Phase 12 — AC coupling on CH B
     * ================================================================ */
    mark_phase(h, 12, "A DC 5V, B AC 5V, 1us");
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 0 /* AC */, PS2000_5V);
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.2);

    /* Restore DC for subsequent phases. */
    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V);

    /* ================================================================
     * Phase 13..16 — sample-interval sweep (A+B DC 5V fixed)
     *   500 ns, 1 us, 2 us, 10 us → isolates timebase bytes.
     * ================================================================ */
    struct { uint32_t iv; PS2000_TIME_UNITS u; const char *name; } ivs[] = {
        { 500, PS2000_NS, "500ns=2MSps" },
        {   2, PS2000_US,    "2us=500kSps" },
        {   5, PS2000_US,    "5us=200kSps" },
        {  10, PS2000_US,    "10us=100kSps" },
    };
    for (unsigned i = 0; i < sizeof(ivs)/sizeof(ivs[0]); i++) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "ival=%s A+B DC 5V", ivs[i].name);
        mark_phase(h, 13 + (int)i, lbl);
        run_stream(h, ivs[i].iv, ivs[i].u, 100000, 0, 1, 50000, 1.2);
    }

    /* ================================================================
     * Phase 17 — max_samples variation
     *   Isolates the sample-count/buffer bytes.
     * ================================================================ */
    mark_phase(h, 17, "max_samples=10000, 1us, A+B DC 5V");
    run_stream(h, 1, PS2000_US, 10000, 0, 1, 50000, 1.2);

    mark_phase(h, 18, "max_samples=1000000, 1us, A+B DC 5V");
    run_stream(h, 1, PS2000_US, 1000000, 0, 1, 50000, 1.2);

    /* ================================================================
     * Phase 19 — overview buffer size variation.
     * ================================================================ */
    mark_phase(h, 19, "overview_buf=1000");
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 1000, 1.2);

    mark_phase(h, 20, "overview_buf=500000");
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 500000, 1.2);

    /* ================================================================
     * Phase 21 — no aggregation.
     * ================================================================ */
    mark_phase(h, 21, "aggregate=0 (raw), 1us");
    run_stream(h, 1, PS2000_US, 100000, 0, 0, 50000, 1.2);

    /* ================================================================
     * Phase 22 — auto_stop enabled.
     * ================================================================ */
    mark_phase(h, 22, "auto_stop=1, 1us, 100k samples");
    run_stream(h, 1, PS2000_US, 100000, 1, 1, 50000, 1.2);

    /* ================================================================
     * Phase 23 — siggen: sine 1 kHz 2 Vpp, 0 V offset.
     * ================================================================ */
    mark_phase(h, 23, "siggen sine 1kHz 2Vpp 0offset, no stream");
    ps2000_set_sig_gen_built_in(h, 0, 2000000, PS2000_SINE,
                                1000.0f, 1000.0f, 0.0f, 0.0f,
                                PS2000_UP, 0);
    usleep(300000);

    /* ================================================================
     * Phase 24 — siggen ON + stream baseline (interaction check).
     * ================================================================ */
    mark_phase(h, 24, "siggen sine 1kHz 2Vpp + stream 1us A+B DC 5V");
    run_stream(h, 1, PS2000_US, 100000, 0, 1, 50000, 1.5);

    /* ================================================================
     * Phase 25 — siggen square, different freq/amp.
     * ================================================================ */
    mark_phase(h, 25, "siggen square 10kHz 4Vpp +500mV offset");
    ps2000_set_sig_gen_built_in(h, 500000, 4000000, PS2000_SQUARE,
                                10000.0f, 10000.0f, 0.0f, 0.0f,
                                PS2000_UP, 0);
    usleep(300000);

    /* ================================================================
     * Phase 26 — siggen DC voltage +1V.
     * ================================================================ */
    mark_phase(h, 26, "siggen DC +1V");
    ps2000_set_sig_gen_built_in(h, 1000000, 0, PS2000_DC_VOLTAGE,
                                0.0f, 0.0f, 0.0f, 0.0f,
                                PS2000_UP, 0);
    usleep(300000);

    /* ================================================================
     * Phase 27 — siggen OFF (pkpk=0, DC 0V).
     * ================================================================ */
    mark_phase(h, 27, "siggen OFF");
    ps2000_set_sig_gen_built_in(h, 0, 0, PS2000_DC_VOLTAGE,
                                0.0f, 0.0f, 0.0f, 0.0f,
                                PS2000_UP, 0);
    usleep(300000);

    /* ================================================================
     * Phase 28 — run_streaming (legacy, NOT _ns) to compare opcode chain.
     * ================================================================ */
    mark_phase(h, 28, "legacy run_streaming(10ms, 1000, 1)");
    if (ps2000_run_streaming(h, 10, 1000, 1)) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            if (dt >= 1.2) break;
            int16_t dummy[2000];
            ps2000_get_values(h, dummy, NULL, NULL, NULL, NULL, 1000);
            usleep(20000);
        }
        ps2000_stop(h);
    } else {
        fprintf(stderr, "  run_streaming(legacy) failed\n");
    }

    /* ================================================================
     * Phase 29 — run_block (to see the block-mode opcode chain).
     * ================================================================ */
    mark_phase(h, 29, "run_block timebase=3 2048 samples");
    int32_t block_ms = 0;
    int16_t ready = 0;
    if (ps2000_run_block(h, 2048, 3, 1, &block_ms)) {
        for (int i = 0; i < 50 && !ready; i++) {
            usleep(20000);
            ready = ps2000_ready(h);
        }
        if (ready) {
            int16_t bufA[2048], bufB[2048];
            int16_t overflow = 0;
            ps2000_get_values(h, bufA, bufB, NULL, NULL, &overflow, 2048);
        }
        ps2000_stop(h);
    } else {
        fprintf(stderr, "  run_block failed\n");
    }

    /* ================================================================
     * Phase 30 — final marker.
     * ================================================================ */
    mark_phase(h, 30, "END");

    ps2000_close_unit(h);
    printf("[param] done.\n");
    return 0;
}
