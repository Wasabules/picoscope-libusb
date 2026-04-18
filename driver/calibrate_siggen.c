/*
 * Full-range calibration using the internal signal generator.
 *
 * Procedure:
 *   1. Enables siggen (1kHz sine)
 *   2. Measures Vpp at 20V range (reference — assumed correct)
 *   3. Measures ADC peak-to-peak at all other ranges
 *   4. Computes effective mV range for each PGA setting
 *
 * Usage: Connect siggen output to Channel A, then:
 *   sudo ./calibrate_siggen
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define N_SAMPLES 4000
#define N_CAPTURES 10  /* average multiple captures */

typedef struct {
    ps_range_t range;
    const char *name;
    int nominal_mv;
} range_info_t;

static const range_info_t RANGES[] = {
    { PS_50MV,  " 50 mV", 50 },
    { PS_100MV, "100 mV", 100 },
    { PS_200MV, "200 mV", 200 },
    { PS_500MV, "500 mV", 500 },
    { PS_1V,    "  1 V",  1000 },
    { PS_2V,    "  2 V",  2000 },
    { PS_5V,    "  5 V",  5000 },
    { PS_10V,   " 10 V",  10000 },
    { PS_20V,   " 20 V",  20000 },
};
#define N_RANGES 9

/* Measure ADC statistics at a given range.
 * Returns 1 on success, 0 on failure. */
static int measure_range(ps2204a_device_t *dev, ps_range_t range, int nominal_mv,
                          double *out_adc_min, double *out_adc_max,
                          double *out_adc_mean, double *out_mv_pp)
{
    ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, range);
    ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
    ps2204a_set_timebase(dev, 5, N_SAMPLES);

    float *buf = (float *)malloc(N_SAMPLES * sizeof(float));
    if (!buf) return 0;

    double global_adc_min = 255;
    double global_adc_max = 0;
    double global_mv_min = 1e9;
    double global_mv_max = -1e9;
    double sum_adc = 0;
    int total = 0;

    float scale = (float)nominal_mv / 128.0f;

    for (int c = 0; c < N_CAPTURES; c++) {
        int actual = 0;
        ps_status_t st = ps2204a_capture_block(dev, N_SAMPLES, buf, NULL, &actual);
        if (st != PS_OK || actual <= 0) continue;

        for (int i = 0; i < actual; i++) {
            /* Reverse-compute ADC from reported mV */
            double adc = (buf[i] / scale) + 128.0;
            sum_adc += adc;
            if (adc < global_adc_min) global_adc_min = adc;
            if (adc > global_adc_max) global_adc_max = adc;
            if (buf[i] < global_mv_min) global_mv_min = buf[i];
            if (buf[i] > global_mv_max) global_mv_max = buf[i];
        }
        total += actual;
    }

    free(buf);
    if (total == 0) return 0;

    *out_adc_min = global_adc_min;
    *out_adc_max = global_adc_max;
    *out_adc_mean = sum_adc / total;
    *out_mv_pp = global_mv_max - global_mv_min;
    return 1;
}

int main(void)
{
    ps2204a_device_t *dev = NULL;

    printf("=== PicoScope 2204A - Signal Generator Calibration ===\n\n");

    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        printf("Failed to open device (status=%d)\n", st);
        return 1;
    }

    /* Enable signal generator: 1kHz sine */
    printf("Enabling signal generator: 1 kHz sine\n");
    st = ps2204a_set_siggen(dev, PS_WAVE_SINE, 1000.0f);
    if (st != PS_OK) {
        printf("WARNING: siggen failed (status=%d), continuing anyway\n", st);
    }

    /* Wait for signal to stabilize */
    usleep(500000);

    printf("Signal generator active. Connect siggen output to Channel A.\n");
    printf("(Waiting 2 seconds for connection...)\n\n");
    sleep(2);

    /* Phase 1: Measure at 20V range (reference) */
    printf("Phase 1: Reference measurement at 20V range\n");
    double ref_adc_min, ref_adc_max, ref_adc_mean, ref_mv_pp;
    if (!measure_range(dev, PS_20V, 20000, &ref_adc_min, &ref_adc_max,
                       &ref_adc_mean, &ref_mv_pp)) {
        printf("FAILED to capture at 20V range\n");
        ps2204a_close(dev);
        return 1;
    }

    double ref_adc_pp = ref_adc_max - ref_adc_min;
    double true_vpp = ref_adc_pp * 20000.0 / 128.0;

    printf("  ADC: min=%.1f  max=%.1f  pp=%.1f  mean=%.1f\n",
           ref_adc_min, ref_adc_max, ref_adc_pp, ref_adc_mean);
    printf("  Signal Vpp (reference) = %.1f mV (%.3f V)\n\n", true_vpp, true_vpp / 1000.0);

    if (ref_adc_pp < 3.0) {
        printf("ERROR: Signal too weak at 20V range (ADC pp = %.1f)\n", ref_adc_pp);
        printf("Make sure siggen output is connected to Channel A.\n");
        ps2204a_close(dev);
        return 1;
    }

    /* Phase 2: Measure at all ranges */
    printf("Phase 2: Cross-calibration across all ranges\n\n");

    printf("%-8s  %-6s  %-8s  %-8s  %-8s  %-10s  %-10s  %-8s\n",
           "Range", "Nom.", "ADC min", "ADC max", "ADC pp", "Eff. range", "Correction", "Clipped?");
    printf("-------  ------  --------  --------  --------  ----------  ----------  --------\n");

    int corrected_mv[N_RANGES];

    for (int r = 0; r < N_RANGES; r++) {
        double adc_min, adc_max, adc_mean, mv_pp;

        if (!measure_range(dev, RANGES[r].range, RANGES[r].nominal_mv,
                           &adc_min, &adc_max, &adc_mean, &mv_pp)) {
            printf("%-8s  %5d   FAILED\n", RANGES[r].name, RANGES[r].nominal_mv);
            corrected_mv[r] = RANGES[r].nominal_mv;
            continue;
        }

        double adc_pp = adc_max - adc_min;
        int clipped = (adc_max > 252 || adc_min < 3);

        if (adc_pp < 2.0) {
            /* Signal too small to measure */
            printf("%-8s  %5d   %7.1f   %7.1f   %7.1f   %-10s  %-10s  %s\n",
                   RANGES[r].name, RANGES[r].nominal_mv,
                   adc_min, adc_max, adc_pp,
                   "TOO SMALL", "N/A", clipped ? "YES" : "no");
            corrected_mv[r] = RANGES[r].nominal_mv;
            continue;
        }

        /* effective_range = true_vpp * 128.0 / adc_pp  (for full-scale ±range) */
        /* But we need to think about this: ADC pp captures peak-to-peak of signal.
         * The "range" means ADC 0-255 maps to ±range_mv.
         * So: Vpp_displayed = adc_pp * 2 * range_mv / 256 = adc_pp * range_mv / 128
         * And: effective_range = true_vpp * 128 / adc_pp
         */
        double effective = true_vpp * 128.0 / adc_pp;
        double correction = effective / RANGES[r].nominal_mv;

        printf("%-8s  %5d   %7.1f   %7.1f   %7.1f   %9.0f   %9.3f   %s\n",
               RANGES[r].name, RANGES[r].nominal_mv,
               adc_min, adc_max, adc_pp,
               effective, correction, clipped ? "YES*" : "no");

        if (clipped) {
            /* Clipped signal — effective range is lower bound only, not accurate */
            /* For clipped ranges, use nominal value (trust SDK design) */
            corrected_mv[r] = RANGES[r].nominal_mv;
        } else {
            corrected_mv[r] = (int)(effective + 0.5);
        }
    }

    /* Print results */
    printf("\n\n=== CALIBRATION RESULTS ===\n");
    printf("Reference signal Vpp = %.1f mV (%.3f V)\n\n", true_vpp, true_vpp / 1000.0);

    printf("/* Calibrated RANGE_MV table for picoscope2204a.c */\n");
    printf("static const int RANGE_MV[] = {\n    ");
    for (int r = 0; r < N_RANGES; r++) {
        printf("%d", corrected_mv[r]);
        if (r < N_RANGES - 1) printf(", ");
    }
    printf("\n};\n");

    /* Print detailed table */
    printf("\n/* PGA settings and calibration per range: */\n");
    static const char *pga_str[] = {
        "(0,7,0)", "(1,6,0)", "(1,7,0)", "(1,2,1)",
        "(1,3,0)", "(1,1,0)", "(0,7,0)", "(0,2,0)", "(0,3,0)"
    };
    for (int r = 0; r < N_RANGES; r++) {
        printf("/*  %-6s  PGA %s  nominal=%5d  calibrated=%5d  ratio=%.3f */\n",
               RANGES[r].name, pga_str[r],
               RANGES[r].nominal_mv, corrected_mv[r],
               (double)corrected_mv[r] / RANGES[r].nominal_mv);
    }

    /* Disable siggen */
    ps2204a_disable_siggen(dev);

    ps2204a_close(dev);
    return 0;
}
