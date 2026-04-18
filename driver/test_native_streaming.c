/* Quick test for native streaming mode in the C driver.
 *
 * Builds with:
 *   gcc -O2 -Wall -o test_native_streaming test_native_streaming.c \
 *       -L. -lpicoscope2204a -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 *
 * Run: ./test_native_streaming
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_cb_count = 0;
static volatile int g_cb_samples = 0;

static void my_stream_cb(const float *a, const float *b, int n, void *ud)
{
    (void)b; (void)a; (void)ud;
    g_cb_count++;
    g_cb_samples += n;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        fprintf(stderr, "Open failed: %d\n", st);
        return 1;
    }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);

    printf("\n--- Starting NATIVE streaming (~100 S/s hardware-limited) ---\n");
    st = ps2204a_start_streaming_mode(dev, PS_STREAM_NATIVE, 0,
                                      my_stream_cb, NULL, 100000);
    if (st != PS_OK) {
        fprintf(stderr, "Start streaming failed: %d\n", st);
        ps2204a_close(dev);
        return 1;
    }

    /* Run for 10 seconds to check sustained rate */
    sleep(10);

    ps_stream_stats_t stats;
    ps2204a_get_streaming_stats(dev, &stats);
    printf("\nStats after 3s: blocks=%llu samples=%llu rate=%.1f S/s\n",
           (unsigned long long)stats.blocks,
           (unsigned long long)stats.total_samples,
           stats.samples_per_sec);
    printf("Callbacks: %d, total samples: %d\n", g_cb_count, g_cb_samples);

    ps2204a_stop_streaming(dev);
    printf("Streaming stopped.\n");

    /* Get last 1000 samples */
    float buf[1000];
    int actual = 0;
    ps2204a_get_streaming_latest(dev, buf, NULL, 1000, &actual);
    printf("Retrieved %d samples from ring.\n", actual);
    if (actual > 0) {
        float sum = 0, min = buf[0], max = buf[0];
        for (int i = 0; i < actual; i++) {
            sum += buf[i];
            if (buf[i] < min) min = buf[i];
            if (buf[i] > max) max = buf[i];
        }
        printf("  Mean=%.2f mV  Min=%.2f mV  Max=%.2f mV\n",
               sum / actual, min, max);
    }

    ps2204a_close(dev);
    return 0;
}
