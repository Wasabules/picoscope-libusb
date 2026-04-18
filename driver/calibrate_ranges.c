/*
 * Calibration: measure ADC raw values at all 9 ranges with same input voltage.
 * This determines the actual effective voltage per PGA setting.
 *
 * Usage: sudo ./calibrate_ranges
 * Connect a stable DC voltage (e.g. battery) to Channel A before running.
 */
#include "picoscope2204a.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_SAMPLES 2000
#define N_CAPTURES 5  /* average over multiple captures */

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

int main(void)
{
    ps2204a_device_t *dev = NULL;

    printf("=== PicoScope 2204A - Range Calibration ===\n");
    printf("Connect a STABLE DC voltage to Channel A.\n\n");

    ps_status_t st = ps2204a_open(&dev);
    if (st != PS_OK) {
        printf("Failed to open device (status=%d)\n", st);
        return 1;
    }

    float *buf = (float *)malloc(N_SAMPLES * sizeof(float));
    if (!buf) { ps2204a_close(dev); return 1; }

    /* Store raw ADC mean for each range */
    double adc_mean[N_RANGES];
    double mv_reported[N_RANGES];
    int valid[N_RANGES];

    printf("%-8s  %-10s  %-10s  %-10s  %-10s  %-10s\n",
           "Range", "ADC mean", "ADC min", "ADC max", "mV (nom)", "mV (20V ref)");
    printf("-------  ----------  ----------  ----------  ----------  ----------\n");

    for (int r = 0; r < N_RANGES; r++) {
        ps2204a_set_channel(dev, PS_CHANNEL_A, true, PS_DC, RANGES[r].range);
        ps2204a_set_channel(dev, PS_CHANNEL_B, false, PS_DC, PS_5V);
        ps2204a_set_timebase(dev, 5, N_SAMPLES);

        double sum_mv = 0;
        double sum_raw = 0;
        double min_raw = 255, max_raw = 0;
        int total_samples = 0;
        valid[r] = 0;

        for (int c = 0; c < N_CAPTURES; c++) {
            int actual = 0;
            st = ps2204a_capture_block(dev, N_SAMPLES, buf, NULL, &actual);
            if (st != PS_OK || actual <= 0) continue;

            /* Reverse-compute ADC raw from mV values */
            float range_mv = (float)RANGES[r].nominal_mv;
            for (int i = 0; i < actual; i++) {
                double adc_raw = (buf[i] / (range_mv / 128.0)) + 128.0;
                sum_raw += adc_raw;
                sum_mv += buf[i];
                if (adc_raw < min_raw) min_raw = adc_raw;
                if (adc_raw > max_raw) max_raw = adc_raw;
            }
            total_samples += actual;
            valid[r] = 1;
        }

        if (valid[r] && total_samples > 0) {
            adc_mean[r] = sum_raw / total_samples;
            mv_reported[r] = sum_mv / total_samples;
            printf("%-8s  %10.1f  %10.1f  %10.1f  %10.1f  (pending)\n",
                   RANGES[r].name, adc_mean[r], min_raw, max_raw, mv_reported[r]);
        } else {
            adc_mean[r] = 128.0;
            mv_reported[r] = 0;
            printf("%-8s  FAILED\n", RANGES[r].name);
        }
    }

    /*
     * Now compute actual effective range using 20V as reference.
     * At 20V range, the reported mV should be approximately correct.
     *
     * For a constant input voltage V:
     *   V = (ADC_raw - 128) * effective_range_mv / 128
     *
     * If 20V range gives ADC_raw_20v for voltage V:
     *   V = (ADC_raw_20v - 128) * 20000 / 128
     *
     * For any other range with ADC_raw_r:
     *   V = (ADC_raw_r - 128) * effective_range_mv_r / 128
     *
     * Therefore:
     *   effective_range_mv_r = V * 128 / (ADC_raw_r - 128)
     *     = (ADC_raw_20v - 128) * 20000 / (ADC_raw_r - 128)
     */

    int ref_idx = 8;  /* 20V */
    double ref_adc_delta = adc_mean[ref_idx] - 128.0;
    double ref_voltage_mv = ref_adc_delta * 20000.0 / 128.0;

    printf("\n\n=== Calibration Results ===\n");
    printf("Reference: 20V range, ADC mean = %.1f (delta = %.1f)\n",
           adc_mean[ref_idx], ref_adc_delta);
    printf("Estimated input voltage: %.1f mV (%.3f V)\n\n",
           ref_voltage_mv, ref_voltage_mv / 1000.0);

    printf("%-8s  %-8s  %-12s  %-12s  %-10s\n",
           "Range", "Nominal", "Effective", "Correction", "New RANGE_MV");
    printf("-------  --------  -----------  -----------  ----------\n");

    /* Output the corrected RANGE_MV table */
    int corrected_mv[N_RANGES];

    for (int r = 0; r < N_RANGES; r++) {
        if (!valid[r]) {
            corrected_mv[r] = RANGES[r].nominal_mv;
            printf("%-8s  %6d    INVALID\n", RANGES[r].name, RANGES[r].nominal_mv);
            continue;
        }

        double adc_delta = adc_mean[r] - 128.0;
        double effective;

        if (adc_delta < 1.0 && adc_delta > -1.0) {
            /* ADC near center — can't calibrate (voltage too low for this range) */
            effective = RANGES[r].nominal_mv;
            printf("%-8s  %6d    %-12s  %-12s  %6d (ADC ~center, using nominal)\n",
                   RANGES[r].name, RANGES[r].nominal_mv, "N/A", "N/A",
                   (int)effective);
        } else {
            effective = ref_voltage_mv * 128.0 / adc_delta;
            if (effective < 0) effective = -effective;  /* Handle sign */
            double correction = effective / RANGES[r].nominal_mv;
            printf("%-8s  %6d    %10.0f    %10.3f    %6d\n",
                   RANGES[r].name, RANGES[r].nominal_mv,
                   effective, correction, (int)(effective + 0.5));
            corrected_mv[r] = (int)(effective + 0.5);
        }
    }

    /* Print C array for copy-paste */
    printf("\n\n/* Corrected RANGE_MV table for picoscope2204a.c */\n");
    printf("static const int RANGE_MV[] = {\n    ");
    for (int r = 0; r < N_RANGES; r++) {
        printf("%d", corrected_mv[r]);
        if (r < N_RANGES - 1) printf(", ");
    }
    printf("\n};\n");

    /* Also print PGA settings for reference */
    printf("\n/* PGA settings per range (for reference): */\n");
    static const char *pga_str[] = {
        "(0,7,0)", "(1,6,0)", "(1,7,0)", "(1,2,1)",
        "(1,3,0)", "(1,1,0)", "(0,7,0)", "(0,2,0)", "(0,3,0)"
    };
    for (int r = 0; r < N_RANGES; r++) {
        printf("/*  %-6s  PGA %s  nominal=%5d  effective=%5d */\n",
               RANGES[r].name, pga_str[r], RANGES[r].nominal_mv, corrected_mv[r]);
    }

    free(buf);
    ps2204a_close(dev);
    return 0;
}
