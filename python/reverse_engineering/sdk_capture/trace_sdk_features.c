/* Minimal SDK harness for USB protocol tracing of specific features.
 *
 * Runs through the SDK API sequentially with no interactive input, so we
 * can capture clean, isolated USB traces under LD_PRELOAD=usb_interceptor.so.
 *
 * Build:
 *   gcc -O2 -Wall -o trace_sdk_features trace_sdk_features.c \
 *       -L/opt/picoscope/lib -lps2000 -Wl,-rpath,/opt/picoscope/lib
 *
 * Run (each phase in its own trace):
 *   LD_PRELOAD=./usb_interceptor.so ./trace_sdk_features 1 >phase1.log 2>&1
 *   mv usb_trace.log trace_advtrig_level.log
 *   LD_PRELOAD=./usb_interceptor.so ./trace_sdk_features 2 >phase2.log 2>&1
 *   mv usb_trace.log trace_advtrig_window.log
 *   ...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <libps2000/ps2000.h>

static int16_t handle;

static void die(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    if (handle > 0) ps2000_close_unit(handle);
    exit(1);
}

static void setup_common(void) {
    handle = ps2000_open_unit();
    if (handle <= 0) die("open_unit");
    printf("[sdk] opened, handle=%d\n", handle);

    ps2000_set_channel(handle, PS2000_CHANNEL_A, 1, 1, PS2000_5V);
    ps2000_set_channel(handle, PS2000_CHANNEL_B, 0, 1, PS2000_5V);
}

/* Phase 1: Basic advanced trigger — LEVEL mode, single channel A rising.
 * This exercises the ps2000SetAdvTrigger* call chain. The USB trace should
 * reveal which opcode/bytes encode advanced trigger state. */
static void phase_advtrig_level(void) {
    setup_common();
    printf("[phase1] Advanced trigger: LEVEL mode, CH A rising @ 1500 mV\n");

    PS2000_TRIGGER_CHANNEL_PROPERTIES props = {
        .thresholdMajor = 1500,
        .thresholdMinor = 0,
        .hysteresis = 1024,
        .channel = PS2000_CHANNEL_A,
        .thresholdMode = PS2000_LEVEL,
    };
    PS2000_TRIGGER_CONDITIONS conds = {
        .channelA = PS2000_CONDITION_TRUE,
        .channelB = PS2000_CONDITION_DONT_CARE,
        .channelC = PS2000_CONDITION_DONT_CARE,
        .channelD = PS2000_CONDITION_DONT_CARE,
        .external = PS2000_CONDITION_DONT_CARE,
        .pulseWidthQualifier = PS2000_CONDITION_DONT_CARE,
    };

    if (!ps2000SetAdvTriggerChannelConditions(handle, &conds, 1)) die("AdvCond");
    if (!ps2000SetAdvTriggerChannelDirections(handle,
            PS2000_ADV_RISING, PS2000_ADV_RISING,
            PS2000_ADV_RISING, PS2000_ADV_RISING, PS2000_ADV_RISING)) die("AdvDir");
    if (!ps2000SetAdvTriggerChannelProperties(handle, &props, 1, 0)) die("AdvProps");

    int32_t time_indisp = 0;
    ps2000_run_block(handle, 1000, 8, 0, &time_indisp);
    usleep(200000);
    int16_t ov = 0;
    int16_t a[1000] = {0}, b[1000] = {0};
    ps2000_get_values(handle, a, b, NULL, NULL, &ov, 1000);
    printf("[phase1] block done\n");
}

/* Phase 2: Window trigger (thresholdMode = WINDOW). */
static void phase_advtrig_window(void) {
    setup_common();
    printf("[phase2] Advanced trigger: WINDOW mode, CH A, 500..1500 mV\n");

    PS2000_TRIGGER_CHANNEL_PROPERTIES props = {
        .thresholdMajor = 1500,
        .thresholdMinor = 500,
        .hysteresis = 512,
        .channel = PS2000_CHANNEL_A,
        .thresholdMode = PS2000_WINDOW,
    };
    PS2000_TRIGGER_CONDITIONS conds = {
        .channelA = PS2000_CONDITION_TRUE,
        .channelB = PS2000_CONDITION_DONT_CARE,
        .channelC = PS2000_CONDITION_DONT_CARE,
        .channelD = PS2000_CONDITION_DONT_CARE,
        .external = PS2000_CONDITION_DONT_CARE,
        .pulseWidthQualifier = PS2000_CONDITION_DONT_CARE,
    };

    ps2000SetAdvTriggerChannelConditions(handle, &conds, 1);
    ps2000SetAdvTriggerChannelDirections(handle,
            PS2000_ENTER, PS2000_ADV_RISING,
            PS2000_ADV_RISING, PS2000_ADV_RISING, PS2000_ADV_RISING);
    ps2000SetAdvTriggerChannelProperties(handle, &props, 1, 0);

    int32_t td;
    ps2000_run_block(handle, 1000, 8, 0, &td);
    usleep(200000);
    int16_t ov; int16_t a[1000], b[1000];
    ps2000_get_values(handle, a, b, NULL, NULL, &ov, 1000);
    printf("[phase2] block done\n");
}

/* Phase 3: Pulse-width qualifier (PWQ). */
static void phase_pwq(void) {
    setup_common();
    printf("[phase3] Pulse-width qualifier, 1-10 µs pulse\n");

    PS2000_TRIGGER_CHANNEL_PROPERTIES props = {
        .thresholdMajor = 1500, .thresholdMinor = 0, .hysteresis = 1024,
        .channel = PS2000_CHANNEL_A, .thresholdMode = PS2000_LEVEL,
    };
    PS2000_TRIGGER_CONDITIONS conds = {
        .channelA = PS2000_CONDITION_TRUE,
        .channelB = PS2000_CONDITION_DONT_CARE,
        .channelC = PS2000_CONDITION_DONT_CARE,
        .channelD = PS2000_CONDITION_DONT_CARE,
        .external = PS2000_CONDITION_DONT_CARE,
        .pulseWidthQualifier = PS2000_CONDITION_TRUE,
    };
    PS2000_PWQ_CONDITIONS pwq_conds = {
        .channelA = PS2000_CONDITION_TRUE,
        .channelB = PS2000_CONDITION_DONT_CARE,
        .channelC = PS2000_CONDITION_DONT_CARE,
        .channelD = PS2000_CONDITION_DONT_CARE,
        .external = PS2000_CONDITION_DONT_CARE,
    };

    ps2000SetAdvTriggerChannelConditions(handle, &conds, 1);
    ps2000SetAdvTriggerChannelDirections(handle,
            PS2000_ADV_RISING, PS2000_ADV_RISING,
            PS2000_ADV_RISING, PS2000_ADV_RISING, PS2000_ADV_RISING);
    ps2000SetAdvTriggerChannelProperties(handle, &props, 1, 0);
    ps2000SetPulseWidthQualifier(handle, &pwq_conds, 1,
                                 PS2000_RISING, 100, 1000,
                                 PS2000_PW_TYPE_IN_RANGE);

    int32_t td;
    ps2000_run_block(handle, 1000, 8, 0, &td);
    usleep(200000);
    int16_t ov; int16_t a[1000], b[1000];
    ps2000_get_values(handle, a, b, NULL, NULL, &ov, 1000);
    printf("[phase3] block done\n");
}

/* Phase 4: ETS (Equivalent-Time Sampling). */
static void phase_ets(void) {
    setup_common();
    printf("[phase4] ETS mode, FAST interleave\n");

    int32_t ets_sample_time_ps;
    ets_sample_time_ps = ps2000_set_ets(handle, PS2000_ETS_FAST, 20, 4);
    printf("[phase4] ETS sample time: %d ps\n", ets_sample_time_ps);

    int16_t a[5000], b[5000];
    int32_t time_values[5000];

    ps2000_set_trigger(handle, PS2000_CHANNEL_A, 32767 / 4,
                       PS2000_RISING, -50, 0);

    int32_t td;
    ps2000_run_block(handle, 5000, 1, 0, &td);
    usleep(500000);
    int16_t ov;
    ps2000_get_times_and_values(handle, time_values, a, b, NULL, NULL,
                                &ov, PS2000_PS, 5000);
    printf("[phase4] ETS block done\n");

    ps2000_set_ets(handle, PS2000_ETS_OFF, 0, 0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <1|2|3|4>\n"
                        "  1 = advanced trigger LEVEL\n"
                        "  2 = advanced trigger WINDOW\n"
                        "  3 = PWQ\n"
                        "  4 = ETS\n", argv[0]);
        return 2;
    }
    int phase = atoi(argv[1]);
    switch (phase) {
        case 1: phase_advtrig_level();  break;
        case 2: phase_advtrig_window(); break;
        case 3: phase_pwq();            break;
        case 4: phase_ets();            break;
        default: fprintf(stderr, "bad phase\n"); return 2;
    }

    if (handle > 0) ps2000_close_unit(handle);
    return 0;
}
