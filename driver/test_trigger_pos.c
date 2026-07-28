/* Where does the trigger actually land in the returned block?
 *
 * Setup: AWG output wired to channel B.
 *
 * The AWG runs slow enough that less than one period fits in the record, so
 * the trace crosses the threshold rising exactly once — and with a working
 * trigger that crossing is the trigger instant. Its index, over the number of
 * samples returned, is the trigger position. delay_pct says where it is meant
 * to be, so the two can be compared directly.
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#define N 2000

/* First rising crossing of `thr`, or -1. Hysteresis of a few mV keeps noise
 * on a slow edge from reporting a crossing early. */
static int rising_cross(const float *x, int n, float thr, float hyst)
{
    int armed = 0;
    for (int i = 0; i < n; i++) {
        if (!armed) { if (x[i] < thr - hyst) armed = 1; }
        else if (x[i] >= thr) return i;
    }
    return -1;
}

int main(int argc, char **argv)
{
    float freq = (argc > 1) ? (float)atof(argv[1]) : 150.0f;
    float thr  = (argc > 2) ? (float)atof(argv[2]) : 0.0f;

    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) { printf("open failed\n"); return 1; }

    /* Square, not sine: the crossing detector needs part of a slow edge to
     * arm, which on a sine shows up as tens of samples of apparent offset
     * that belong to the measurement, not the device. */
    ps2204a_set_siggen(dev, PS_WAVE_SQUARE, freq, 2000000);
    usleep(400000);
    ps2204a_set_channel(dev, PS_CHANNEL_A, false, PS_DC, PS_2V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true,  PS_DC, PS_2V);
    ps2204a_set_timebase(dev, 5, N);
    usleep(300000);

    printf("\nAWG %.0f Hz on B, %d samples at 320 ns = %.2f ms "
           "(%.2f period)\n", freq, N, N * 320e-6, N * 320e-9 * freq);
    printf("threshold %.0f mV rising\n\n", thr);
    printf("  %-10s %-14s %-14s %s\n",
           "delay_pct", "expected pos", "measured pos", "trigger index");
    printf("  ------------------------------------------------------------\n");

    static float b[8192];
    const float delays[] = { -80, -50, 0, 50, 80 };

    for (unsigned k = 0; k < sizeof(delays) / sizeof(delays[0]); k++) {
        float dp = delays[k];
        ps2204a_set_trigger(dev, PS_CHANNEL_B, thr, PS_RISING, dp, 2000);
        usleep(200000);
        int got = 0;
        ps2204a_capture_block(dev, N, NULL, b, &got);          /* settle */
        if (ps2204a_capture_block(dev, N, NULL, b, &got) != PS_OK || got < 100) {
            printf("  %-10.0f capture failed\n", dp);
            continue;
        }
        int idx = rising_cross(b, got, thr, 20.0f);
        float want = (100.0f - dp) / 200.0f;
        if (idx < 0) {
            printf("  %-10.0f %-14.1f%% %-14s %s\n", dp, want * 100,
                   "no crossing", "-");
        } else {
            printf("  %-10.0f %-14.1f%% %-13.1f%% %d/%d\n",
                   dp, want * 100, 100.0f * idx / got, idx, got);
        }
    }

    ps2204a_disable_trigger(dev);
    ps2204a_disable_siggen(dev);
    ps2204a_close(dev);
    return 0;
}
