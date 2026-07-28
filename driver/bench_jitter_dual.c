/* Trigger-jitter bench for dual-channel block capture.
 *
 * Setup: AWG output wired to channel B. Channel A is left unconnected on
 * purpose — see below.
 *
 * Build:
 *   gcc -O2 -Wall -o bench_jitter_dual bench_jitter_dual.c \
 *       -L. -lpicoscope2204a -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 *
 * Method. A working trigger locks every capture to the same point of a
 * repetitive waveform, so successive captures overlay. Taking the sample-wise
 * standard deviation across R captures gives the overlay spread in millivolts;
 * dividing by the mean absolute step between adjacent samples converts that
 * into the unit that matters, samples of timing jitter. The conversion is what
 * makes runs at different amplitudes and slew rates comparable — a bigger
 * signal inflates the spread and the slope together.
 *
 * Read the jitter column, not the dispersion column. Comparing dispersion
 * across configurations is how the AWG amplitude bug hid for as long as it did.
 *
 * The unconnected channel A is a control on the interleaved decode. The wire
 * format is B,A,B,A anchored on the end of the transfer, so an off-by-one in
 * the de-interleave puts B's signal into A. A reading tens of millivolts is
 * the front-end noise floor and means the decode is clean; A reading hundreds
 * means it is not, whatever the B column says.
 *
 * Expect roughly 0.9 / 0.5 / 1.2 samples on the three stimuli, matching the
 * official SDK to within the run-to-run spread (bench_jitter_dual_sdk.c is the
 * same measurement through libps2000). Single runs vary by ±0.1 or so, so
 * repeat before reading anything into a small difference.
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#define N 2000
#define R 8

/* Mean over sample index of the standard deviation across repeats. */
static float overlay_spread(float caps[R][N], int n)
{
    double acc = 0;
    for (int i = 0; i < n; i++) {
        double s = 0, s2 = 0;
        for (int r = 0; r < R; r++) { s += caps[r][i]; s2 += (double)caps[r][i] * caps[r][i]; }
        double m = s / R;
        acc += sqrt(fmax(0.0, s2 / R - m * m));
    }
    return (float)(acc / n);
}

static float mean_step(const float *x, int n)
{
    double a = 0;
    for (int i = 1; i < n; i++) a += fabs(x[i] - x[i - 1]);
    return (float)(a / (n - 1));
}

static float rms_about_mean(const float *x, int n)
{
    double s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += x[i]; s2 += (double)x[i] * x[i]; }
    double m = s / n;
    return (float)sqrt(fmax(0.0, s2 / n - m * m));
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) { printf("open failed\n"); return 1; }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_2V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_2V);

    struct { const char *n; ps_wave_t w; float f; } stim[] = {
        { "square 10 kHz", PS_WAVE_SQUARE, 10000 },
        { "sine   10 kHz", PS_WAVE_SINE,   10000 },
        { "sine    2 kHz", PS_WAVE_SINE,    2000 },
    };

    static float cap_b[R][N], cap_a[R][N];
    printf("\n%-15s %-10s %-10s %-11s %-12s %s\n",
           "stimulus", "RMS B", "RMS A", "step/sample", "spread mV", "jitter (samples)");

    for (unsigned k = 0; k < sizeof(stim) / sizeof(stim[0]); k++) {
        ps2204a_set_siggen(dev, stim[k].w, stim[k].f, 2000000);
        usleep(400000);
        ps2204a_set_timebase(dev, 5, N);
        /* delay_pct 0 puts the trigger mid-record, matching the SDK's -50. */
        ps2204a_set_trigger(dev, PS_CHANNEL_B, 0.0f, PS_RISING, 0.0f, 2000);
        usleep(200000);

        int n_got = 0;
        for (int r = 0; r < R; r++) {
            int got = 0;
            if (ps2204a_capture_block(dev, N, cap_a[r], cap_b[r], &got) != PS_OK) got = 0;
            n_got = got;
        }
        if (n_got < 100) { printf("%-15s empty capture\n", stim[k].n); continue; }

        float sig  = rms_about_mean(cap_b[0], n_got);
        float idle = rms_about_mean(cap_a[0], n_got);
        float step = mean_step(cap_b[0], n_got);
        float spread = overlay_spread(cap_b, n_got);
        printf("%-15s %-10.1f %-10.1f %-11.2f %-12.1f %.2f\n",
               stim[k].n, sig, idle, step, spread, step > 0 ? spread / step : 0);
    }

    ps2204a_disable_trigger(dev);
    ps2204a_disable_siggen(dev);
    ps2204a_close(dev);
    return 0;
}
