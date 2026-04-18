/* Verify PS2204A against datasheet specs:
 *   - 100 MS/s single channel (block mode, tb=0)
 *   - 50 MS/s dual channel (block mode, tb=1)
 *   - Timebase range 10 ns/sample (tb=0) up to the driver's clamp
 *   - Max samples per block (datasheet says 8 kS)
 *   - 1 MS/s streaming (PS_STREAM_SDK, already verified)
 *
 * Build:
 *   gcc -O2 -Wall -o bench_specs bench_specs.c -L. -lpicoscope2204a \
 *       -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static double ts_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void bench_block(ps2204a_device_t *dev, int tb, int samples, bool dual,
                        const char *label)
{
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, dual, PS_DC, PS_5V);
    ps2204a_disable_trigger(dev);

    int max = ps2204a_max_samples(dev);
    if (samples > max) samples = max;
    ps2204a_set_timebase(dev, tb, samples);

    float *a = (float *)malloc(samples * sizeof(float));
    float *b = dual ? (float *)malloc(samples * sizeof(float)) : NULL;

    /* Warmup (prime the capture path). */
    int actual = 0;
    ps2204a_capture_block(dev, samples, a, b, &actual);

    int N = 20;
    double t0 = ts_now();
    int total_samples = 0;
    for (int i = 0; i < N; i++) {
        ps_status_t st = ps2204a_capture_block(dev, samples, a, b, &actual);
        if (st != PS_OK) {
            printf("  [%s] capture failed: status=%d (iter %d)\n",
                   label, st, i);
            break;
        }
        total_samples += actual;
    }
    double dt = ts_now() - t0;

    int interval_ns = ps2204a_timebase_to_ns(tb);
    double theo_rate = 1e9 / interval_ns;
    double samples_per_run = (double)total_samples / N;

    /* Compute signal stats on last block — confirms ADC actually running */
    float mn = a[0], mx = a[0];
    double sum = 0;
    for (int i = 0; i < actual; i++) {
        if (a[i] < mn) mn = a[i];
        if (a[i] > mx) mx = a[i];
        sum += a[i];
    }

    printf("  [%s] tb=%d (%d ns/sample = %.1f MS/s theoretical)\n",
           label, tb, interval_ns, theo_rate / 1e6);
    printf("    samples/block=%.0f dual=%s  %d blocks in %.2f s\n",
           samples_per_run, dual ? "yes" : "no", N, dt);
    printf("    throughput = %.1f kS/s (wall-clock, host-limited)\n",
           total_samples / dt / 1e3);
    printf("    last block: min=%.1f max=%.1f mean=%.1f mV (span=%.1f)\n",
           mn, mx, sum / actual, mx - mn);

    free(a);
    if (b) free(b);
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    printf("=== PS2204A datasheet spec verification ===\n\n");

    printf("-- Single-channel block capture at max rate --\n");
    bench_block(dev, 0, 8064, false, "100 MS/s 1ch");
    bench_block(dev, 1, 8064, false, " 50 MS/s 1ch");
    bench_block(dev, 3, 8064, false, " 12.5 MS/s 1ch");

    printf("\n-- Dual-channel block capture at max rate --\n");
    bench_block(dev, 1, 3968, true,  " 50 MS/s 2ch");
    bench_block(dev, 2, 3968, true,  " 25 MS/s 2ch");

    printf("\n-- Max sample count per block --\n");
    int try_sizes[] = {8064, 16384, 32768, 65536, 131072, 262144};
    for (unsigned i = 0; i < sizeof(try_sizes)/sizeof(try_sizes[0]); i++) {
        ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
        ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
        ps2204a_disable_trigger(dev);
        ps2204a_set_timebase(dev, 3, try_sizes[i]);
        float *a = (float *)malloc(try_sizes[i] * sizeof(float));
        int actual = 0;
        ps_status_t st = ps2204a_capture_block(dev, try_sizes[i], a, NULL, &actual);
        printf("  requested=%d  actual=%d  status=%d\n",
               try_sizes[i], actual, st);
        free(a);
    }

    printf("\n-- Timebase range --\n");
    for (int tb = 0; tb <= 23; tb++) {
        int ns = ps2204a_timebase_to_ns(tb);
        printf("  tb=%2d : %11d ns/sample = %9.3f %s/sample\n",
               tb, ns,
               ns < 1000 ? (double)ns : ns < 1e6 ? ns/1e3 : ns < 1e9 ? ns/1e6 : ns/1e9,
               ns < 1000 ? "ns" : ns < 1e6 ? "µs" : ns < 1e9 ? "ms" : "s");
    }

    ps2204a_close(dev);
    return 0;
}
