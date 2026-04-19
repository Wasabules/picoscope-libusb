/*
 * Minimal libps2000 SDK streaming driver. Built to run under
 * LD_PRELOAD=libps_intercept.so so we can capture a byte-exact trace of
 * what the official driver does on a fresh cold-plug.
 *
 *   gcc sdk_stream_trace.c -o sdk_stream_trace \
 *       -I/opt/picoscope/include/libps2000 \
 *       -L/opt/picoscope/lib -lps2000 \
 *       -Wl,-rpath,/opt/picoscope/lib
 *
 *   LD_PRELOAD=$PWD/../firmware-extractor/libps_intercept.so \
 *       ./sdk_stream_trace
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "ps2000.h"

static volatile long total_samples = 0;

static void PREF4 stream_callback(int16_t **buffers,
                                  int16_t  overflow,
                                  uint32_t triggeredAt,
                                  int16_t  triggered,
                                  int16_t  auto_stop,
                                  uint32_t n_values)
{
    (void)buffers; (void)overflow; (void)triggeredAt;
    (void)triggered; (void)auto_stop;
    total_samples += (long)n_values;
}

int main(void)
{
    printf("[sdk] opening unit...\n");
    int16_t h = ps2000_open_unit();
    if (h <= 0) { fprintf(stderr, "ps2000_open_unit failed: %d\n", h); return 1; }
    printf("[sdk] handle=%d\n", h);

    int8_t info[64];
    ps2000_get_unit_info(h, info, sizeof(info), 4); /* batch/serial */
    printf("[sdk] serial=%s\n", info);

    /* CH A + CH B both on (matches our PS_STREAM_SDK profile). */
    if (!ps2000_set_channel(h, PS2000_CHANNEL_A, 1, 1, PS2000_5V)) {
        fprintf(stderr, "set_channel A failed\n"); return 1;
    }
    if (!ps2000_set_channel(h, PS2000_CHANNEL_B, 1, 1, PS2000_5V)) {
        fprintf(stderr, "set_channel B failed\n"); return 1;
    }

    printf("[sdk] starting streaming 1us (1 MS/s)...\n");
    /* sample_interval=1, PS2000_US => 1 us/sample = 1 MS/s.
     * auto_stop=0, aggregate=1, overview buffer=1000. */
    if (!ps2000_run_streaming_ns(h, 1, PS2000_US, 100000, 0, 1, 50000)) {
        fprintf(stderr, "ps2000_run_streaming_ns failed\n");
        ps2000_close_unit(h); return 1;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (dt >= 3.0) break;
        ps2000_get_streaming_last_values(h, stream_callback);
        usleep(20000);
    }

    printf("[sdk] total samples ~%ld over 3s\n", total_samples);
    ps2000_stop(h);
    ps2000_close_unit(h);
    printf("[sdk] done.\n");
    return 0;
}
