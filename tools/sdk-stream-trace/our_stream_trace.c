/*
 * Equivalent to sdk_stream_trace, but driven through our libpicoscope2204a
 * driver in PS_STREAM_SDK mode. Run under LD_PRELOAD=libps_intercept.so to
 * capture a byte-level trace of our streaming sequence for diffing against
 * the SDK trace.
 *
 *   gcc our_stream_trace.c -o our_stream_trace \
 *       -I../../driver ../../driver/libpicoscope2204a.a -lusb-1.0 -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "picoscope2204a.h"

int main(void)
{
    printf("[our] opening device...\n");
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        fprintf(stderr, "ps2204a_open failed: %d\n", st);
        return 1;
    }
    printf("[our] open OK\n");

    /* Match the SDK profile: CH A + CH B both on, ±5V DC. */
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_5V);

    printf("[our] starting PS_STREAM_SDK 1us...\n");
    st = ps2204a_start_streaming_mode(dev, PS_STREAM_SDK,
                                      1 /* us */, NULL, NULL, 0);
    if (st != PS_OK) {
        fprintf(stderr, "start_streaming_mode failed: %d\n", st);
        ps2204a_close(dev);
        return 1;
    }

    /* Let it stream for 3 s. */
    sleep(3);

    ps_stream_stats_t stats = {0};
    ps2204a_get_streaming_stats(dev, &stats);
    printf("[our] samples=%lu sps=%.0f blocks=%lu\n",
           (unsigned long)stats.total_samples,
           stats.samples_per_sec,
           (unsigned long)stats.blocks);

    ps2204a_stop_streaming(dev);
    ps2204a_close(dev);
    printf("[our] done.\n");
    return 0;
}
