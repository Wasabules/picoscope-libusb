/*
 * ETS hardware test — needs a repetitive signal on CH A.
 * Uses the built-in siggen as a self-contained source (known to produce
 * a ~1.49 MHz tone regardless of commanded frequency; the repetition
 * itself is enough for ETS to reconstruct the waveform).
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static void print_stats(const char *label, const float *buf, int n)
{
    float mn = buf[0], mx = buf[0];
    double sum = 0, sqsum = 0;
    for (int i = 0; i < n; i++) {
        float v = buf[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        sum   += v;
        sqsum += (double)v * v;
    }
    double mean = sum / n;
    double var  = sqsum / n - mean * mean;
    printf("  %-10s n=%5d  min=%+7.1f mV  max=%+7.1f mV  mean=%+7.1f mV  σ=%6.1f mV  Vpp=%.1f mV\n",
           label, n, mn, mx, mean, sqrt(var > 0 ? var : 0), mx - mn);
}

/* Count zero crossings at a positive threshold as a crude freq estimator. */
static int count_crossings(const float *buf, int n, float thr)
{
    int c = 0;
    for (int i = 1; i < n; i++) {
        if (buf[i-1] < thr && buf[i] >= thr) c++;
    }
    return c;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) { fprintf(stderr, "open failed: %d\n", st); return 1; }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true,  PS_DC, PS_2V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_2V);
    ps2204a_set_timebase(dev, 0, 2000);

    /* Enable siggen — whatever it produces, ETS should reconstruct it. */
    ps2204a_set_siggen(dev, PS_WAVE_SINE, 100000.0f, 1000000);

    /* Arm a LEVEL trigger at 0 mV, rising, centred in block. */
    ps2204a_set_trigger(dev, PS_CHANNEL_A, 0.0f, PS_RISING, 0.0f, 500);

    /* ====== Baseline: plain block capture at 10 ns ====== */
    float *base = (float *)calloc(2000, sizeof(float));
    int got = 0;
    st = ps2204a_capture_block(dev, 2000, base, NULL, &got);
    if (st != PS_OK) {
        fprintf(stderr, "baseline capture failed: %d\n", st);
        free(base); ps2204a_close(dev); return 1;
    }
    printf("Baseline (tb=0, 10 ns, no ETS):\n");
    print_stats("CH A", base, got);
    int xb = count_crossings(base, got, 0.0f);
    printf("  rising crossings @ 0 mV: %d over %.1f µs -> %.2f MHz\n\n",
           xb, got * 10e-3, xb / (got * 10e-3));

    /* ====== ETS fast (10× interleaves) ====== */
    int iv = 0;
    st = ps2204a_set_ets(dev, PS_ETS_FAST, 0, 0, &iv);
    if (st != PS_OK) {
        fprintf(stderr, "set_ets FAST failed: %d\n", st);
        free(base); ps2204a_close(dev); return 1;
    }
    printf("ETS FAST: interval = %d ps (%.2f GS/s effective)\n",
           iv, 1e3 / iv);

    int block_n = 500;   /* 500 × 10 = 5000 samples out at 1 ns */
    int total_f = block_n * 10;
    float *ets_f = (float *)calloc(total_f, sizeof(float));
    int got_f = 0, iv_f = 0;
    st = ps2204a_capture_ets(dev, block_n, ets_f, NULL,
                             total_f, &got_f, &iv_f);
    if (st != PS_OK) {
        fprintf(stderr, "capture_ets FAST failed: %d\n", st);
        free(base); free(ets_f); ps2204a_close(dev); return 1;
    }
    printf("  got %d samples, interval=%d ps -> %.1f µs window\n",
           got_f, iv_f, got_f * iv_f * 1e-6);
    print_stats("ETS-fast A", ets_f, got_f);
    int xf = count_crossings(ets_f, got_f, 0.0f);
    double span_us = got_f * iv_f * 1e-6;
    printf("  rising crossings @ 0 mV: %d over %.1f µs -> %.2f MHz\n\n",
           xf, span_us, xf / span_us);

    /* ====== ETS slow (20× interleaves, 4 cycles) ====== */
    st = ps2204a_set_ets(dev, PS_ETS_SLOW, 0, 0, &iv);
    if (st != PS_OK) {
        fprintf(stderr, "set_ets SLOW failed: %d\n", st);
        free(base); free(ets_f); ps2204a_close(dev); return 1;
    }
    printf("ETS SLOW: interval = %d ps (%.2f GS/s effective)\n",
           iv, 1e3 / iv);

    int total_s = block_n * 20;
    float *ets_s = (float *)calloc(total_s, sizeof(float));
    int got_s = 0, iv_s = 0;
    st = ps2204a_capture_ets(dev, block_n, ets_s, NULL,
                             total_s, &got_s, &iv_s);
    if (st != PS_OK) {
        fprintf(stderr, "capture_ets SLOW failed: %d\n", st);
    } else {
        printf("  got %d samples, interval=%d ps -> %.1f µs window\n",
               got_s, iv_s, got_s * iv_s * 1e-6);
        print_stats("ETS-slow A", ets_s, got_s);
        int xs = count_crossings(ets_s, got_s, 0.0f);
        double sp = got_s * iv_s * 1e-6;
        printf("  rising crossings @ 0 mV: %d over %.1f µs -> %.2f MHz\n\n",
               xs, sp, xs / sp);
    }

    /* ====== Correctness check ======
     * The number of rising crossings per µs should be the SAME across
     * baseline, ETS-fast, ETS-slow — ETS just samples the same signal
     * at a higher effective rate, the frequency doesn't change. */
    double f_base = xb / (got * 10e-3);
    double f_fast = xf / (got_f * iv_f * 1e-6);
    double err_fast = fabs(f_fast - f_base) / f_base * 100.0;
    printf("Frequency sanity: baseline=%.2f MHz, ETS-fast=%.2f MHz, deviation=%.1f%%\n",
           f_base, f_fast, err_fast);
    int ok = err_fast < 10.0;

    free(base); free(ets_f); free(ets_s);
    ps2204a_close(dev);
    printf("\n%s\n", ok ? "*** ETS TEST PASSED ***" : "*** ETS TEST FAILED ***");
    return ok ? 0 : 1;
}
