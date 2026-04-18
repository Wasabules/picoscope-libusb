/* Verify pre-/post-trigger delay places the trigger edge at the expected
 * position inside the returned block.
 *
 * Method: siggen drives a ~200 Hz square wave on CH A (period 5 ms) and we
 * capture 8064 samples at tb=5 (320 ns/sample → block spans 2.58 ms). With
 * half-period > block length we see at most one rising edge per capture,
 * and its position is dictated by where the trigger fires relative to the
 * returned window:
 *
 *    delay_pct = +100  → block starts at trigger → edge near sample 0
 *    delay_pct =    0  → block centred on trigger → edge near N/2
 *    delay_pct = -100  → block ends at trigger → edge near N-1
 *
 * Tolerance: ±10 % of N. The hardware only exposes sample-level post-trigger
 * count, so extreme delays can be off by up to one 16 KB raw-buffer slice.
 *
 * Build:
 *   gcc -O2 -Wall -o bench_trigger_delay bench_trigger_delay.c \
 *       -L. -lpicoscope2204a -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#define N 8064

static int first_rising_zero_cross(const float *buf, int n)
{
    for (int i = 1; i < n; i++) {
        if (buf[i - 1] < 0.0f && buf[i] >= 0.0f) return i;
    }
    return -1;
}

static int run_case(ps2204a_device_t *dev, float delay_pct, float *buf)
{
    ps2204a_set_trigger(dev, PS_CHANNEL_A, 0.0f, PS_RISING, delay_pct, 2000);

    int actual = 0;
    ps_status_t st = ps2204a_capture_block(dev, N, buf, NULL, &actual);
    if (st != PS_OK) {
        printf("  delay=%+4.0f%%: capture failed (status=%d)\n",
               delay_pct, st);
        return -1;
    }

    int idx = first_rising_zero_cross(buf, actual);
    /* Parser places trigger at pre_wanted = n*(100-dp)/200 samples into the
     * output → that's the expected sample index of the trigger edge. */
    int expected = (int)((100.0f - delay_pct) / 200.0f * (float)actual);

    float mn = buf[0], mx = buf[0];
    int lows = 0, highs = 0;
    for (int i = 0; i < actual; i++) {
        if (buf[i] < mn) mn = buf[i];
        if (buf[i] > mx) mx = buf[i];
        if (buf[i] < -500) lows++;
        if (buf[i] >  500) highs++;
    }

    int tol = (int)(actual * 0.10f);
    int delta = idx < 0 ? -1 : abs(idx - expected);
    const char *verdict = idx < 0 ? "NO CROSS" :
                          delta <= tol ? "OK" : "OFF";

    printf("  dp=%+4.0f%%  edge=%5d  expect=%5d  d=%+5d  tol=±%d  "
           "span=%.0f..%.0fmV  lo/hi=%d/%d  %s\n",
           delay_pct, idx, expected, idx < 0 ? 0 : (idx - expected),
           tol, mn, mx, lows, highs, verdict);

    return idx;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true,  PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, N);

    /* 200 Hz square wave, 2 Vpp centred on 0 V. Half-period 2.5 ms > block
     * span 2.58 ms is *nearly* equal — drop to 150 Hz so half-period is
     * ~3.33 ms, comfortably larger than the 2.58 ms block. */
    if (ps2204a_set_siggen(dev, PS_WAVE_SQUARE, 150.0f, 2000000) != PS_OK) {
        fprintf(stderr, "siggen setup failed\n");
        ps2204a_close(dev);
        return 1;
    }
    usleep(500000);  /* settle + ensure trigger catches an edge */

    float *buf = (float *)malloc(N * sizeof(float));
    if (!buf) { ps2204a_close(dev); return 1; }

    printf("=== Pre/post-trigger delay verification ===\n");
    printf("Signal: 150 Hz square, 2 Vpp on CH A @ 5V range\n");
    printf("Capture: tb=5 (320 ns/sample), N=%d samples (2.58 ms)\n", N);
    printf("Trigger: CH A rising @ 0 V\n\n");

    float cases[] = { +100.0f, +50.0f, 0.0f, -50.0f, -100.0f };
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        run_case(dev, cases[i], buf);
    }

    ps2204a_disable_siggen(dev);
    free(buf);
    ps2204a_close(dev);
    return 0;
}
