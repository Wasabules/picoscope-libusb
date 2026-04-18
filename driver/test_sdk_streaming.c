/* Quick test for PS_STREAM_SDK (gap-free 1 MS/s) mode in the C driver.
 *
 * Build:
 *   gcc -O2 -Wall -o test_sdk_streaming test_sdk_streaming.c \
 *       -L. -lpicoscope2204a -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile long g_cb_calls = 0;
static volatile long g_cb_samples = 0;

static void my_cb(const float *a, const float *b, int n, void *ud)
{
    (void)a; (void)b; (void)ud;
    __sync_fetch_and_add(&g_cb_calls, 1);
    __sync_fetch_and_add(&g_cb_samples, n);
}

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 3;

    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        fprintf(stderr, "Open failed: %d\n", st);
        return 1;
    }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true,  PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true,  PS_DC, PS_5V);

    printf("\n--- Starting SDK streaming (1 MS/s dual-channel gap-free) ---\n");
    st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK, 1,
                                      my_cb, NULL, 4 * 1024 * 1024);
    if (st != PS_OK) {
        fprintf(stderr, "Start streaming failed: %d\n", st);
        ps2204a_close(dev);
        return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sleep(secs);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    ps_stream_stats_t stats;
    ps2204a_get_streaming_stats(dev, &stats);
    printf("\n--- Results after %.2f s ---\n", elapsed);
    printf("  blocks=%llu  samples=%llu  rate=%.1f kS/s  last_block=%.2f ms\n",
           (unsigned long long)stats.blocks,
           (unsigned long long)stats.total_samples,
           stats.samples_per_sec / 1000.0,
           stats.last_block_ms);
    printf("  Callbacks: %ld, samples delivered: %ld\n",
           g_cb_calls, g_cb_samples);
    printf("  dt_ns = %d\n", ps2204a_get_streaming_dt_ns(dev));

    ps2204a_stop_streaming(dev);

    /* Sample some data */
    int N = 10000;
    float *a = (float *)malloc(N * sizeof(float));
    float *b = (float *)malloc(N * sizeof(float));
    int actual = 0;
    ps2204a_get_streaming_latest(dev, a, b, N, &actual);
    if (actual > 0) {
        float sa = 0, sb = 0, mina = a[0], maxa = a[0], minb = b[0], maxb = b[0];
        for (int i = 0; i < actual; i++) {
            sa += a[i];
            sb += b[i];
            if (a[i] < mina) mina = a[i];
            if (a[i] > maxa) maxa = a[i];
            if (b[i] < minb) minb = b[i];
            if (b[i] > maxb) maxb = b[i];
        }
        printf("  CH A: mean=%.2f min=%.2f max=%.2f mV\n",
               sa / actual, mina, maxa);
        printf("  CH B: mean=%.2f min=%.2f max=%.2f mV\n",
               sb / actual, minb, maxb);
        printf("  First 16 A samples: ");
        for (int i = 0; i < 16 && i < actual; i++) printf("%.0f ", a[i]);
        printf("\n  First 16 B samples: ");
        for (int i = 0; i < 16 && i < actual; i++) printf("%.0f ", b[i]);
        printf("\n");
    }
    free(a); free(b);

    ps2204a_close(dev);
    return 0;
}
