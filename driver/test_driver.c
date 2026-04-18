/*
 * PicoScope 2204A C driver — test/validation program
 *
 * Usage: sudo ./test_driver
 */

#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define MAX_SAMPLES 8064

static double time_ms(struct timespec *a, struct timespec *b)
{
    return (b->tv_sec - a->tv_sec) * 1000.0 +
           (b->tv_nsec - a->tv_nsec) / 1e6;
}

/* ======================================================================== */
/* Test 1: Device info                                                      */
/* ======================================================================== */
static int test_info(ps2204a_device_t *dev)
{
    char serial[32], cal_date[32];
    ps2204a_get_info(dev, serial, sizeof(serial), cal_date, sizeof(cal_date));
    printf("\n--- Test 1: Device Info ---\n");
    printf("  Serial:   %s\n", serial);
    printf("  Cal Date: %s\n", cal_date);
    return 0;
}

/* ======================================================================== */
/* Test 2: Basic block capture                                              */
/* ======================================================================== */
static int test_basic_capture(ps2204a_device_t *dev)
{
    printf("\n--- Test 2: Basic Block Capture (tb=5, 1000 samples) ---\n");

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, 1000);

    float *buf = (float *)malloc(MAX_SAMPLES * sizeof(float));
    if (!buf) return -1;

    int actual = 0;
    ps_status_t st = ps2204a_capture_block(dev, 1000, buf, NULL, &actual);

    if (st != PS_OK) {
        printf("  FAIL: capture returned %d\n", st);
        free(buf);
        return -1;
    }

    printf("  OK: %d samples captured\n", actual);

    /* Basic stats */
    float min_v = buf[0], max_v = buf[0], sum = 0;
    for (int i = 0; i < actual; i++) {
        if (buf[i] < min_v) min_v = buf[i];
        if (buf[i] > max_v) max_v = buf[i];
        sum += buf[i];
    }
    float mean = sum / actual;
    printf("  Min=%.1f mV, Max=%.1f mV, Mean=%.1f mV\n", min_v, max_v, mean);

    free(buf);
    return 0;
}

/* ======================================================================== */
/* Test 3: Consecutive captures                                             */
/* ======================================================================== */
static int test_consecutive(ps2204a_device_t *dev)
{
    printf("\n--- Test 3: 10 Consecutive Captures ---\n");

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, MAX_SAMPLES);

    float *buf = (float *)malloc(MAX_SAMPLES * sizeof(float));
    if (!buf) return -1;

    int ok = 0, fail = 0;
    for (int i = 0; i < 10; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        int actual = 0;
        ps_status_t st = ps2204a_capture_block(dev, MAX_SAMPLES, buf, NULL, &actual);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = time_ms(&t0, &t1);

        if (st == PS_OK && actual > 0) {
            ok++;
            if (i < 3 || i == 9) {
                printf("  Capture %2d: %d samples, %.0f ms\n", i+1, actual, dt);
            }
        } else {
            fail++;
            printf("  Capture %2d: FAIL (status=%d)\n", i+1, st);
        }
    }

    printf("  Result: %d/10 OK, %d/10 FAIL\n", ok, fail);
    free(buf);
    return fail > 0 ? -1 : 0;
}

/* ======================================================================== */
/* Test 4: All timebases                                                    */
/* ======================================================================== */
static int test_timebases(ps2204a_device_t *dev)
{
    printf("\n--- Test 4: Timebases 0-10 ---\n");

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);

    float *buf = (float *)malloc(MAX_SAMPLES * sizeof(float));
    if (!buf) return -1;

    int ok = 0;
    for (int tb = 0; tb <= 10; tb++) {
        ps2204a_set_timebase(dev, tb, MAX_SAMPLES);

        int actual = 0;
        ps_status_t st = ps2204a_capture_block(dev, MAX_SAMPLES, buf, NULL, &actual);

        int interval_ns = ps2204a_timebase_to_ns(tb);
        if (st == PS_OK && actual > 0) {
            ok++;
            printf("  tb=%2d (%5d ns): %d samples [OK]\n", tb, interval_ns, actual);
        } else {
            printf("  tb=%2d (%5d ns): FAIL (status=%d)\n", tb, interval_ns, st);
        }
    }

    printf("  Result: %d/11 OK\n", ok);
    free(buf);
    return ok < 11 ? -1 : 0;
}

/* ======================================================================== */
/* Test 5: All ranges                                                       */
/* ======================================================================== */
static int test_ranges(ps2204a_device_t *dev)
{
    printf("\n--- Test 5: All Voltage Ranges ---\n");

    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, MAX_SAMPLES);

    float *buf = (float *)malloc(MAX_SAMPLES * sizeof(float));
    if (!buf) return -1;

    static const ps_range_t ranges[] = {
        PS_50MV, PS_100MV, PS_200MV, PS_500MV, PS_1V,
        PS_2V, PS_5V, PS_10V, PS_20V
    };
    static const char *names[] = {
        "50mV", "100mV", "200mV", "500mV", "1V",
        "2V", "5V", "10V", "20V"
    };

    int ok = 0;
    for (int i = 0; i < 9; i++) {
        ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, ranges[i]);

        int actual = 0;
        ps_status_t st = ps2204a_capture_block(dev, MAX_SAMPLES, buf, NULL, &actual);

        if (st == PS_OK && actual > 0) {
            ok++;
            float mean = 0;
            for (int j = 0; j < actual; j++) mean += buf[j];
            mean /= actual;
            printf("  %-5s: %d samples, mean=%.1f mV [OK]\n",
                   names[i], actual, mean);
        } else {
            printf("  %-5s: FAIL (status=%d)\n", names[i], st);
        }
    }

    printf("  Result: %d/9 OK\n", ok);
    free(buf);

    /* Restore default */
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    return ok < 9 ? -1 : 0;
}

/* ======================================================================== */
/* Test 6: Streaming                                                        */
/* ======================================================================== */
static void stream_callback(const float *data_a, const float *data_b,
                            int n, void *user)
{
    (void)data_a; (void)data_b; (void)n; (void)user;
}

static int test_streaming(ps2204a_device_t *dev)
{
    printf("\n--- Test 6: Streaming (3 seconds) ---\n");

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);

    /* Use sample_interval_us=1 for maximum throughput (maps to tb=7,
     * clamped by the driver). At tb=0 (10ns, 100 MSPS), 8064 samples
     * take only 80µs to capture, so block overhead dominates. */
    ps_status_t st = ps2204a_start_streaming(dev, 1, stream_callback,
                                              NULL, 0);
    if (st != PS_OK) {
        printf("  FAIL: start_streaming returned %d\n", st);
        return -1;
    }

    /* Print stats each second */
    for (int s = 1; s <= 3; s++) {
        sleep(1);
        ps_stream_stats_t stats;
        ps2204a_get_streaming_stats(dev, &stats);
        printf("  %ds: %llu blocks, %llu samples, %.0f kS/s, "
               "last=%.1f ms/block\n",
               s,
               (unsigned long long)stats.blocks,
               (unsigned long long)stats.total_samples,
               stats.samples_per_sec / 1000.0,
               stats.last_block_ms);
    }

    ps2204a_stop_streaming(dev);

    ps_stream_stats_t final_stats;
    ps2204a_get_streaming_stats(dev, &final_stats);
    printf("  Total: %llu samples in %.1f s = %.0f kS/s\n",
           (unsigned long long)final_stats.total_samples,
           final_stats.elapsed_s,
           final_stats.samples_per_sec / 1000.0);

    return final_stats.total_samples > 100000 ? 0 : -1;
}

/* ======================================================================== */
/* Test 7: Dual-channel capture                                             */
/* ======================================================================== */
static void stats_f(const float *x, int n, float *min_v, float *max_v,
                    float *mean, float *stddev)
{
    if (n <= 0) { *min_v = *max_v = *mean = *stddev = 0; return; }
    float mn = x[0], mx = x[0], sum = 0;
    for (int i = 0; i < n; i++) {
        if (x[i] < mn) mn = x[i];
        if (x[i] > mx) mx = x[i];
        sum += x[i];
    }
    float m = sum / n;
    float var = 0;
    for (int i = 0; i < n; i++) {
        float d = x[i] - m;
        var += d * d;
    }
    *min_v = mn; *max_v = mx; *mean = m; *stddev = sqrtf(var / n);
}

static int test_dual_channel(ps2204a_device_t *dev)
{
    printf("\n--- Test 7: Dual Channel Capture ---\n");

    /* Match SDK reference config (test_sdk_dual_capture.py):
     * CH A = 5V, CH B = 500mV, tb=5, 1000 samples. */
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_500MV);
    ps2204a_set_timebase(dev, 5, 1000);

    const int n = 1000;
    float *buf_a = (float *)malloc(MAX_SAMPLES * sizeof(float));
    float *buf_b = (float *)malloc(MAX_SAMPLES * sizeof(float));
    if (!buf_a || !buf_b) { free(buf_a); free(buf_b); return -1; }

    int actual = 0;
    ps_status_t st = ps2204a_capture_block(dev, n, buf_a, buf_b, &actual);
    if (st != PS_OK || actual < 100) {
        printf("  FAIL: capture returned %d, actual=%d\n", st, actual);
        free(buf_a); free(buf_b);
        return -1;
    }

    float mn_a, mx_a, m_a, sd_a;
    float mn_b, mx_b, m_b, sd_b;
    stats_f(buf_a, actual, &mn_a, &mx_a, &m_a, &sd_a);
    stats_f(buf_b, actual, &mn_b, &mx_b, &m_b, &sd_b);

    printf("  A (  5V): min=%7.1f max=%7.1f mean=%7.1f std=%6.1f mV\n",
           mn_a, mx_a, m_a, sd_a);
    printf("  B (500mV): min=%7.1f max=%7.1f mean=%7.1f std=%6.1f mV\n",
           mn_b, mx_b, m_b, sd_b);
    printf("  SDK ref:   A mean=-28 std=21 mV,  B mean=-1.7 std=2.2 mV\n");

    /* Cross-check: capture each channel alone, compare noise level.
     * If our dual parser is correct, per-channel noise std should match
     * the single-channel std to within ~30% (hardware gain mixed in). */
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_500MV);
    ps2204a_capture_block(dev, n, buf_a, NULL, &actual);
    float _mn, _mx, m_a_solo, sd_a_solo;
    stats_f(buf_a, actual, &_mn, &_mx, &m_a_solo, &sd_a_solo);

    ps2204a_set_channel(dev, PS_CHANNEL_A, false, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_500MV);
    ps2204a_capture_block(dev, n, NULL, buf_b, &actual);
    float m_b_solo, sd_b_solo;
    stats_f(buf_b, actual, &_mn, &_mx, &m_b_solo, &sd_b_solo);

    printf("  A-only:    mean=%7.1f std=%6.1f mV (cross-check)\n",
           m_a_solo, sd_a_solo);
    printf("  B-only:    mean=%7.1f std=%6.1f mV (cross-check)\n",
           m_b_solo, sd_b_solo);

    /* Restore dual enabled */
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, true, PS_DC, PS_500MV);

    /* Match criterion: dual noise should be within 30% of solo noise. */
    int ok_a = sd_a_solo > 0 ? fabsf(sd_a - sd_a_solo) / sd_a_solo < 0.30f : 1;
    int ok_b = sd_b_solo > 0 ? fabsf(sd_b - sd_b_solo) / sd_b_solo < 0.30f : 1;

    if (ok_a && ok_b) {
        printf("  OK: per-channel noise matches single-channel reference\n");
    } else {
        printf("  FAIL: dual parse does not match single-channel reference\n");
    }

    free(buf_a); free(buf_b);
    return (ok_a && ok_b) ? 0 : -1;
}

/* ======================================================================== */
/* Test 8: Signal generator                                                 */
/* ======================================================================== */
static int test_siggen(ps2204a_device_t *dev)
{
    printf("\n--- Test 7: Signal Generator ---\n");

    ps_status_t st = ps2204a_set_siggen(dev, PS_WAVE_SINE, 1000.0f, 1000000);
    if (st != PS_OK) {
        printf("  FAIL: set_siggen returned %d\n", st);
        return -1;
    }
    printf("  1 kHz sine — OK\n");

    usleep(500000);

    st = ps2204a_disable_siggen(dev);
    if (st != PS_OK) {
        printf("  FAIL: disable_siggen returned %d\n", st);
        return -1;
    }
    printf("  Disabled — OK\n");

    return 0;
}

/* ======================================================================== */
/* Main                                                                     */
/* ======================================================================== */
int main(void)
{
    ps2204a_device_t *dev = NULL;

    printf("PicoScope 2204A C Driver — Test Suite\n");
    printf("======================================\n");

    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        printf("Failed to open device (status=%d)\n", st);
        return 1;
    }

    int failures = 0;

    failures += (test_info(dev) != 0);
    failures += (test_basic_capture(dev) != 0);
    failures += (test_consecutive(dev) != 0);
    failures += (test_timebases(dev) != 0);
    failures += (test_ranges(dev) != 0);
    failures += (test_streaming(dev) != 0);
    failures += (test_dual_channel(dev) != 0);
    failures += (test_siggen(dev) != 0);

    printf("\n======================================\n");
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
    } else {
        printf("%d TEST(S) FAILED\n", failures);
    }
    printf("======================================\n");

    ps2204a_close(dev);
    return failures;
}
