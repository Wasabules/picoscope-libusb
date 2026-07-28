/* Characterise the channel-B level trigger against a known AWG signal.
 *
 * Setup: AWG output wired to the channel B input.
 *
 * The CH B trigger bytes were extrapolated from the CH A trace rather than
 * captured from one (see the comment above build_capture_cmd2), so this
 * measures whether they actually work, and how the requested threshold maps
 * onto the level the device really triggers at.
 *
 * Method — no reference instrument needed:
 *
 *   Phase stability. A working trigger locks every capture to the same point
 *   of a repetitive waveform, so successive captures overlay exactly. A
 *   trigger that never fires leaves the device free-running and the phase
 *   walks. Taking N captures and averaging the sample-wise standard deviation
 *   across them separates the two without knowing anything about the signal:
 *   near zero means locked, near the signal's own RMS means free-running.
 *
 *   Level accuracy. When locked, the sample at the trigger instant should sit
 *   at the requested threshold. Reading it back gives the real transfer curve
 *   of threshold_mv -> device level, which is what the encoding has to get
 *   right.
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define N_SAMPLES 2000
#define N_REPEAT  6

static float rms_about_mean(const float *x, int n)
{
    double s = 0, s2 = 0;
    for (int i = 0; i < n; i++) { s += x[i]; s2 += (double)x[i] * x[i]; }
    double m = s / n;
    return (float)sqrt(fmax(0.0, s2 / n - m * m));
}

/* Mean over sample index of the standard deviation across repeats. */
static float overlay_spread(float (*caps)[N_SAMPLES], int reps, int n)
{
    double acc = 0;
    for (int i = 0; i < n; i++) {
        double s = 0, s2 = 0;
        for (int r = 0; r < reps; r++) { s += caps[r][i]; s2 += (double)caps[r][i] * caps[r][i]; }
        double m = s / reps;
        acc += sqrt(fmax(0.0, s2 / reps - m * m));
    }
    return (float)(acc / n);
}

int main(int argc, char **argv)
{
    float freq = (argc > 1) ? (float)atof(argv[1]) : 10000.0f;
    ps_range_t range = PS_2V;

    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) { printf("open failed\n"); return 1; }

    printf("\nAWG: sine %.0f Hz -> channel B, range 2 V\n", freq);
    ps2204a_set_siggen(dev, PS_WAVE_SINE, freq, 2000000);
    usleep(400000);

    ps2204a_set_channel(dev, PS_CHANNEL_A, false, PS_DC, range);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true,  PS_DC, range);
    ps2204a_set_timebase(dev, 5, N_SAMPLES);
    usleep(300000);

    static float caps[N_REPEAT][N_SAMPLES];
    int got = 0;

    /* Free-running reference: how much does the phase walk with no trigger? */
    ps2204a_disable_trigger(dev);
    usleep(200000);
    ps2204a_capture_block(dev, N_SAMPLES, NULL, caps[0], &got);
    for (int r = 0; r < N_REPEAT; r++)
        ps2204a_capture_block(dev, N_SAMPLES, NULL, caps[r], &got);
    float signal_rms = rms_about_mean(caps[0], got);
    float free_spread = overlay_spread(caps, N_REPEAT, got);
    printf("\nfree-running : signal RMS = %.1f mV, overlay spread = %.1f mV\n",
           signal_rms, free_spread);
    printf("(a locked trigger should drive the spread far below the RMS)\n\n");

    printf("  %-10s %-12s %-12s %-10s %s\n",
           "thr req", "spread mV", "locked?", "level@t0", "err");
    printf("  ---------------------------------------------------------------\n");

    const float thresholds[] = { -600, -400, -200, -100, 0, 100, 200, 400, 600 };
    for (unsigned k = 0; k < sizeof(thresholds) / sizeof(thresholds[0]); k++) {
        float thr = thresholds[k];
        if (ps2204a_set_trigger(dev, PS_CHANNEL_B, thr, PS_RISING, 0.0f, 2000) != PS_OK) {
            printf("  %-10.0f set_trigger failed\n", thr);
            continue;
        }
        usleep(200000);
        ps2204a_capture_block(dev, N_SAMPLES, NULL, caps[0], &got);   /* settle */
        for (int r = 0; r < N_REPEAT; r++) {
            if (ps2204a_capture_block(dev, N_SAMPLES, NULL, caps[r], &got) != PS_OK) {
                got = 0; break;
            }
        }
        if (got <= 0) { printf("  %-10.0f capture failed\n", thr); continue; }

        float spread = overlay_spread(caps, N_REPEAT, got);
        int locked = spread < signal_rms * 0.25f;
        /* With delay_pct = 0 the trigger sits mid-record. */
        float level = caps[0][got / 2];
        printf("  %-10.0f %-12.1f %-12s %-10.1f %+.1f\n",
               thr, spread, locked ? "yes" : "NO", level, level - thr);
    }

    ps2204a_disable_trigger(dev);
    ps2204a_disable_siggen(dev);
    ps2204a_close(dev);
    return 0;
}
