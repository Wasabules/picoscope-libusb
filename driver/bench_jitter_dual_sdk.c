/* The dual-channel jitter bench of bench_jitter_dual.c, run through the
 * official SDK instead of this driver, so the two numbers are comparable.
 *
 * Setup: AWG output wired to channel B, channel A unconnected — identical to
 * the driver-side bench, and the columns mean the same things. Read its header
 * for the method and for what the channel A column is checking.
 *
 * Build (needs the PicoScope SDK installed):
 *   gcc -O2 -Wall -I/opt/picoscope/include -L/opt/picoscope/lib \
 *       -o bench_jitter_dual_sdk bench_jitter_dual_sdk.c -lps2000 -lm
 *   LD_LIBRARY_PATH=/opt/picoscope/lib ./bench_jitter_dual_sdk
 *
 * Only one device can be open at a time, so run this and the driver-side bench
 * one after the other, not together.
 *
 * The SDK's `delay` is -50 % where ours is delay_pct 0: both put the trigger
 * in the middle of the record. The SDK returns 16-bit values scaled to
 * ±32767 regardless of the 8-bit wire format, hence the conversion below.
 */
#include <libps2000/ps2000.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#define N 2000
#define R 8

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
    int16_t h = ps2000_open_unit();
    if (h <= 0) { printf("open_unit failed (%d)\n", h); return 1; }

    ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_2V);
    ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_2V);

    struct { const char *n; PS2000_WAVE_TYPE w; float f; } stim[] = {
        { "square 10 kHz", PS2000_SQUARE, 10000 },
        { "sine   10 kHz", PS2000_SINE,   10000 },
        { "sine    2 kHz", PS2000_SINE,    2000 },
    };

    static float cap_b[R][N], cap_a[R][N];
    static int16_t a16[N], b16[N];
    printf("\n%-15s %-10s %-10s %-11s %-12s %s\n",
           "stimulus", "RMS B", "RMS A", "step/sample", "spread mV", "jitter (samples)");

    for (unsigned k = 0; k < sizeof(stim) / sizeof(stim[0]); k++) {
        ps2000_set_sig_gen_built_in(h, 0, 2000000, stim[k].w, stim[k].f, stim[k].f,
                                    0, 0, PS2000_UP, 0);
        usleep(400000);
        ps2000_set_trigger(h, PS2000_CHANNEL_B, 0, PS2000_RISING, -50, 2000);

        int n_got = 0;
        for (int r = 0; r < R; r++) {
            int32_t indisposed = 0;
            ps2000_run_block(h, N, 5, 1, &indisposed);
            for (int i = 0; i < 200 && !ps2000_ready(h); i++) usleep(5000);
            int16_t ovf = 0;
            int32_t n = ps2000_get_values(h, a16, b16, NULL, NULL, &ovf, N);
            if (n <= 0) n = 0;
            n_got = (int)n;
            for (int i = 0; i < n_got; i++) {
                cap_b[r][i] = b16[i] * 2000.0f / 32767.0f;
                cap_a[r][i] = a16[i] * 2000.0f / 32767.0f;
            }
        }
        if (n_got < 100) { printf("%-15s empty capture\n", stim[k].n); continue; }

        float sig  = rms_about_mean(cap_b[0], n_got);
        float idle = rms_about_mean(cap_a[0], n_got);
        float step = mean_step(cap_b[0], n_got);
        float spread = overlay_spread(cap_b, n_got);
        printf("%-15s %-10.1f %-10.1f %-11.2f %-12.1f %.2f\n",
               stim[k].n, sig, idle, step, spread, step > 0 ? spread / step : 0);
    }

    ps2000_close_unit(h);
    return 0;
}
