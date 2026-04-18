/* Verify enhanced-resolution (software oversampling) reduces noise by
 * 2^extra_bits as expected.
 *
 * Method: capture N samples on a floating CH A input (ADC sees mostly
 * gaussian noise). Compute standard deviation; enabling extra_bits of
 * oversampling should reduce σ by ~2^extra_bits (the box-filter equivalent
 * resolution gain). The effective bit count is log2(range_mv / σ) − 1.
 *
 * Build:
 *   gcc -O2 -Wall -o bench_enhanced_res bench_enhanced_res.c \
 *       -L. -lpicoscope2204a -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>

#define N 8064

static double stddev(const float *buf, int n)
{
    double sum = 0, sum2 = 0;
    for (int i = 0; i < n; i++) { sum += buf[i]; sum2 += buf[i] * buf[i]; }
    double mean = sum / n;
    return sqrt(sum2 / n - mean * mean);
}

static double mean(const float *buf, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++) s += buf[i];
    return s / n;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true,  PS_DC, PS_20V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, N);
    ps2204a_disable_trigger(dev);

    float *buf = (float *)malloc(N * sizeof(float));
    if (!buf) { ps2204a_close(dev); return 1; }

    /* 20 V range: 1 LSB = 20000/128 ≈ 156 mV. Floating input sits within
     * the range (no rail clipping). Quantization + thermal noise yields
     * σ ≈ 1-3 LSB at 0 extra bits. */
    double range_mv = 20000.0;

    printf("=== Enhanced resolution verification ===\n");
    printf("CH A @ 20V range, tb=5, N=%d samples (free-run)\n", N);
    printf("Expected σ reduction per extra bit: 2× (box-filter theory)\n\n");

    printf("extra_bits | taps | mean_mV  |  σ_mV    | σ_LSB | eff_bits | σ_ratio\n");
    printf("-----------+------+----------+----------+-------+----------+--------\n");

    double sigma0 = 0;
    for (int eb = 0; eb <= 4; eb++) {
        ps2204a_set_resolution_enhancement(dev, eb);

        /* Warm-up */
        int actual = 0;
        ps2204a_capture_block(dev, N, buf, NULL, &actual);

        /* Average σ over 5 captures to smooth out run-to-run variance */
        double ssum = 0, msum = 0;
        const int R = 5;
        for (int r = 0; r < R; r++) {
            ps2204a_capture_block(dev, N, buf, NULL, &actual);
            ssum += stddev(buf, actual);
            msum += mean(buf, actual);
        }
        double sigma = ssum / R;
        double mn = msum / R;

        int taps = 1;
        for (int i = 0; i < eb; i++) taps *= 4;

        double lsb_mv = range_mv / 128.0;
        double sigma_lsb = sigma / lsb_mv;
        /* effective bits = log2(range_mv_pkpk / σ) where pkpk = 2*range_mv */
        double eff_bits = log2(2.0 * range_mv / (sigma * 6.6));  /* 6.6σ ≈ 1 LSB rms-to-pkpk */

        if (eb == 0) sigma0 = sigma;
        double ratio = sigma0 / sigma;

        printf("  %d       | %4d | %+6.3f  |  %6.4f |  %4.2f |   %5.2f  |  %4.2fx\n",
               eb, taps, mn, sigma, sigma_lsb, eff_bits, ratio);
    }

    free(buf);
    ps2204a_close(dev);
    return 0;
}
