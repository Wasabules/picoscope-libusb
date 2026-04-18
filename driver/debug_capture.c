/*
 * Quick diagnostic: capture raw ADC data and print values
 * Usage: sudo ./debug_capture
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_SAMPLES 1000

int main(void)
{
    ps2204a_device_t *dev = NULL;

    printf("=== PicoScope 2204A - Diagnostic Capture ===\n\n");

    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        printf("Failed to open device (status=%d)\n", st);
        return 1;
    }

    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, N_SAMPLES);

    float *buf = (float *)malloc(N_SAMPLES * sizeof(float));
    if (!buf) { ps2204a_close(dev); return 1; }

    int actual = 0;
    st = ps2204a_capture_block(dev, N_SAMPLES, buf, NULL, &actual);

    if (st != PS_OK) {
        printf("Capture failed (status=%d)\n", st);
        free(buf);
        ps2204a_close(dev);
        return 1;
    }

    printf("Captured %d samples at 5V range, DC coupling\n\n", actual);

    /* Print first 30 values */
    printf("First 30 mV values:\n");
    for (int i = 0; i < 30 && i < actual; i++) {
        printf("  [%3d] %8.1f mV", i, buf[i]);
        if ((i + 1) % 5 == 0) printf("\n");
    }
    printf("\n");

    /* Print last 10 values */
    printf("Last 10 mV values:\n");
    for (int i = actual - 10; i < actual; i++) {
        if (i < 0) continue;
        printf("  [%3d] %8.1f mV", i, buf[i]);
        if ((i - actual + 10 + 1) % 5 == 0) printf("\n");
    }
    printf("\n");

    /* Stats */
    float min_v = buf[0], max_v = buf[0], sum = 0;
    for (int i = 0; i < actual; i++) {
        if (buf[i] < min_v) min_v = buf[i];
        if (buf[i] > max_v) max_v = buf[i];
        sum += buf[i];
    }
    float mean = sum / actual;

    printf("Stats:\n");
    printf("  Min  = %.2f mV\n", min_v);
    printf("  Max  = %.2f mV\n", max_v);
    printf("  Mean = %.2f mV\n", mean);
    printf("  Vpp  = %.2f mV\n", max_v - min_v);

    /* Reverse-compute what ADC values would produce these mV */
    printf("\nReverse ADC (5V range, scale=5000/128=39.0625):\n");
    printf("  Mean ADC raw = %.1f (expected ~128 + V*128/5000)\n",
           mean / (5000.0f / 128.0f) + 128.0f);
    printf("  Min  ADC raw = %.1f\n", min_v / (5000.0f / 128.0f) + 128.0f);
    printf("  Max  ADC raw = %.1f\n", max_v / (5000.0f / 128.0f) + 128.0f);

    /* Also test at 20V range */
    printf("\n--- Now testing at 20V range ---\n");
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_20V);
    ps2204a_set_timebase(dev, 5, N_SAMPLES);

    actual = 0;
    st = ps2204a_capture_block(dev, N_SAMPLES, buf, NULL, &actual);
    if (st == PS_OK && actual > 0) {
        float min20 = buf[0], max20 = buf[0], sum20 = 0;
        for (int i = 0; i < actual; i++) {
            if (buf[i] < min20) min20 = buf[i];
            if (buf[i] > max20) max20 = buf[i];
            sum20 += buf[i];
        }
        float mean20 = sum20 / actual;
        printf("  Captured %d samples at 20V range\n", actual);
        printf("  Min  = %.2f mV\n", min20);
        printf("  Max  = %.2f mV\n", max20);
        printf("  Mean = %.2f mV (%.3f V)\n", mean20, mean20 / 1000.0);
        printf("  ADC raw mean = %.1f\n", mean20 / (20000.0f / 128.0f) + 128.0f);
    }

    free(buf);
    ps2204a_close(dev);
    return 0;
}
