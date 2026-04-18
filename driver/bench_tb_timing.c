/* Measure the ADC's actual sample rate by isolating block-acquisition wall
 * time across timebases. At tb=0 (100 MS/s) a 8064-sample block takes ~81 µs
 * of ADC time; at tb=20 (~10.5 ms/sample) it takes ~85 s. USB transfer
 * overhead is roughly constant, so the delta between two tb's is the ADC
 * contribution — a linear fit vs 2^tb confirms the formula 10 × 2^tb ns.
 *
 * Build:
 *   gcc -O2 -Wall -o bench_tb_timing bench_tb_timing.c -L. -lpicoscope2204a \
 *       -lusb-1.0 -lm -lpthread -Wl,-rpath,'$ORIGIN'
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double ts_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;
    if (ps2204a_open(&dev) != PS_OK) return 1;
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, PS_5V);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_disable_trigger(dev);

    int n = 8064;
    float *a = (float *)malloc(n * sizeof(float));

    printf("tb | formula_ns | measured_ms | expected_adc_ms | "
           "overhead_ms | effective_MSPS\n");
    printf("---+------------+-------------+-----------------+"
           "-------------+---------------\n");

    for (int tb = 0; tb <= 15; tb++) {
        ps2204a_set_timebase(dev, tb, n);
        /* Warmup */
        int actual = 0;
        ps2204a_capture_block(dev, n, a, NULL, &actual);

        int reps = (tb <= 5) ? 10 : (tb <= 10 ? 5 : 2);
        double t0 = ts_now();
        for (int i = 0; i < reps; i++)
            ps2204a_capture_block(dev, n, a, NULL, &actual);
        double dt = (ts_now() - t0) / reps * 1000.0;

        double formula_ns = 10.0 * (1ULL << tb);
        double expected_adc_ms = formula_ns * n / 1e6;
        double overhead = dt - expected_adc_ms;
        double effective_msps = n / (dt * 1e-3) / 1e6;

        printf("%2d | %10.0f | %11.2f | %15.2f | %11.2f | %14.3f\n",
               tb, formula_ns, dt, expected_adc_ms, overhead, effective_msps);
    }

    free(a);
    ps2204a_close(dev);
    return 0;
}
