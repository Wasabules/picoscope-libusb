/*
 * JNI wrapper over the reverse-engineered PicoScope 2204A libusb driver.
 * Consumed by io.github.wasabules.ps2204.PicoScope2204A.
 */

/* setenv() is POSIX, not ISO C, so a strict -std=c11 build does not get its
 * prototype from <stdlib.h>. State the POSIX level we rely on rather than
 * depend on whichever default the toolchain happens to pick. Must precede
 * every include. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <jni.h>
#include <stdlib.h>
#include <android/log.h>
#include "picoscope2204a.h"

#define JNI_FN(name) Java_io_github_wasabules_ps2204_PicoScope2204A_##name
#define LOG_TAG "ps2204a"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

/* Open / close ----------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetFirmwareDir)(JNIEnv *env, jclass cls, jstring path)
{
    (void)cls;
    if (!path) return -1;
    const char *c = (*env)->GetStringUTFChars(env, path, NULL);
    int rc = setenv("PS2204A_FIRMWARE_DIR", c, 1);
    LOGI("firmware dir set to %s (rc=%d)", c, rc);
    (*env)->ReleaseStringUTFChars(env, path, c);
    return (jint)rc;
}

JNIEXPORT jlong JNICALL
JNI_FN(nativeOpen)(JNIEnv *env, jclass cls, jint usb_fd)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open_with_fd(&dev, (int)usb_fd);
    if (st != PS_OK) {
        LOGE("ps2204a_open_with_fd failed, status=%d", (int)st);
        return 0;
    }
    return (jlong)(intptr_t)dev;
}

JNIEXPORT jlong JNICALL
JNI_FN(nativeOpenStage1)(JNIEnv *env, jclass cls, jint usb_fd)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open_fd_stage1(&dev, (int)usb_fd);
    if (st != PS_OK) {
        LOGE("ps2204a_open_fd_stage1 failed, status=%d", (int)st);
        return 0;
    }
    LOGI("stage1 complete, device is re-enumerating");
    return (jlong)(intptr_t)dev;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeOpenStage2)(JNIEnv *env, jclass cls, jlong handle, jint new_fd)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    ps_status_t st = ps2204a_open_fd_stage2(dev, (int)new_fd);
    if (st != PS_OK) {
        LOGE("ps2204a_open_fd_stage2 failed, status=%d", (int)st);
    } else {
        LOGI("stage2 complete, device ready");
    }
    return (jint)st;
}

JNIEXPORT void JNICALL
JNI_FN(nativeClose)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (dev) ps2204a_close(dev);
}

/* Configuration ---------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetChannel)(JNIEnv *env, jclass cls, jlong handle,
                         jint channel, jboolean enabled,
                         jint coupling, jint range)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_channel(dev, (ps_channel_t)channel,
                                     (bool)enabled,
                                     (ps_coupling_t)coupling,
                                     (ps_range_t)range);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeSetTimebase)(JNIEnv *env, jclass cls, jlong handle,
                          jint timebase, jint samples)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_timebase(dev, (int)timebase, (int)samples);
}

/* Block capture ---------------------------------------------------------- */

JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeCaptureBlock)(JNIEnv *env, jclass cls, jlong handle, jint samples)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev) return NULL;

    float *buf = (float *)malloc((size_t)samples * sizeof(float));
    if (!buf) return NULL;

    int actual = 0;
    ps_status_t st = ps2204a_capture_block(dev, (int)samples, buf, NULL, &actual);
    if (st != PS_OK || actual <= 0) {
        free(buf);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0, actual, buf);
    }

    free(buf);
    return result;
}

JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeCaptureBlockDual)(JNIEnv *env, jclass cls, jlong handle, jint samples)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || samples <= 0) return NULL;

    float *a = (float *)malloc((size_t)samples * sizeof(float));
    float *b = (float *)malloc((size_t)samples * sizeof(float));
    if (!a || !b) { free(a); free(b); return NULL; }

    int actual = 0;
    ps_status_t st = ps2204a_capture_block(dev, (int)samples, a, b, &actual);
    if (st != PS_OK || actual <= 0) {
        free(a); free(b);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, 2 * actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0, actual, a);
        (*env)->SetFloatArrayRegion(env, result, actual, actual, b);
    }
    free(a); free(b);
    return result;
}

/* Calibration ------------------------------------------------------------ */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetRangeCalibration)(JNIEnv *env, jclass cls, jlong handle,
                                  jint range, jfloat offset_mv, jfloat gain)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_range_calibration(dev, (ps_range_t)range,
                                               offset_mv, gain);
}

/**
 * @return float[]{offset_mv, gain} on success, null on failure.
 */
JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeGetRangeCalibration)(JNIEnv *env, jclass cls, jlong handle,
                                  jint range)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev) return NULL;
    float off = 0.f, g = 1.f;
    if (ps2204a_get_range_calibration(dev, (ps_range_t)range, &off, &g) != PS_OK) {
        return NULL;
    }
    jfloatArray arr = (*env)->NewFloatArray(env, 2);
    if (!arr) return NULL;
    jfloat vals[2] = { off, g };
    (*env)->SetFloatArrayRegion(env, arr, 0, 2, vals);
    return arr;
}

/* Trigger ---------------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetTrigger)(JNIEnv *env, jclass cls, jlong handle,
                         jint source, jfloat threshold_mv, jint direction,
                         jfloat delay_pct, jint auto_trigger_ms)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_trigger(dev, (ps_channel_t)source, threshold_mv,
                                     (ps_trigger_dir_t)direction, delay_pct,
                                     (int)auto_trigger_ms);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeDisableTrigger)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_disable_trigger(dev);
}

/* Streaming -------------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeStartStreaming)(JNIEnv *env, jclass cls, jlong handle, jint interval_us)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_start_streaming(dev, (int)interval_us, NULL, NULL, 0);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeStartStreamingMode)(JNIEnv *env, jclass cls, jlong handle,
                                 jint mode, jint interval_us)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    /* 8 M samples ≈ 8 s at 1 MS/s (SDK mode) → covers time/div presets up to
     * 800 ms/div without hitting the ring ceiling. Memory budget:
     * 8M × 4 bytes × 2 channels ≈ 64 MB in the driver ring. */
    return (jint)ps2204a_start_streaming_mode(dev, (ps_stream_mode_t)mode,
                                              (int)interval_us, NULL, NULL,
                                              8 * 1024 * 1024);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeGetStreamingDtNs)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_get_streaming_dt_ns(dev);
}

/**
 * Returns {blocks, total_samples, elapsed_s×1000, samples_per_sec,
 *          blocks_per_sec, last_block_ms}. 64-bit fields are downcast
 *          to double so one jdoubleArray carries everything.
 */
JNIEXPORT jdoubleArray JNICALL
JNI_FN(nativeGetStreamingStats)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev) return NULL;
    ps_stream_stats_t s = {0};
    if (ps2204a_get_streaming_stats(dev, &s) != PS_OK) return NULL;
    jdoubleArray arr = (*env)->NewDoubleArray(env, 6);
    if (!arr) return NULL;
    jdouble vals[6] = {
        (jdouble)s.blocks,
        (jdouble)s.total_samples,
        s.elapsed_s,
        s.samples_per_sec,
        s.blocks_per_sec,
        s.last_block_ms,
    };
    (*env)->SetDoubleArrayRegion(env, arr, 0, 6, vals);
    return arr;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeStopStreaming)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_stop_streaming(dev);
}

JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeGetLatest)(JNIEnv *env, jclass cls, jlong handle, jint n)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || n <= 0) return NULL;

    float *buf = (float *)malloc((size_t)n * sizeof(float));
    if (!buf) return NULL;

    int actual = 0;
    ps_status_t st = ps2204a_get_streaming_latest(dev, buf, NULL, (int)n, &actual);
    if (st != PS_OK || actual <= 0) {
        free(buf);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0, actual, buf);
    }

    free(buf);
    return result;
}

/**
 * Dual-channel latest fetch. Returns a flat jfloatArray of length 2*actual:
 *   [ A[0..actual-1], B[0..actual-1] ]
 * Caller slices on the Java side. Returns null on failure or if no data.
 */
JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeGetLatestDual)(JNIEnv *env, jclass cls, jlong handle, jint n)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || n <= 0) return NULL;

    float *a = (float *)malloc((size_t)n * sizeof(float));
    float *b = (float *)malloc((size_t)n * sizeof(float));
    if (!a || !b) { free(a); free(b); return NULL; }

    int actual = 0;
    ps_status_t st = ps2204a_get_streaming_latest(dev, a, b, (int)n, &actual);
    if (st != PS_OK || actual <= 0) {
        free(a); free(b);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, 2 * actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0, actual, a);
        (*env)->SetFloatArrayRegion(env, result, actual, actual, b);
    }

    free(a); free(b);
    return result;
}

/* Signal generator ------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSiggen)(JNIEnv *env, jclass cls, jlong handle,
                       jint wave_type, jfloat freq_hz, jint pkpk_uv)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    LOGI("nativeSetSiggen wave=%d freq=%.2f pkpk_uv=%d", (int)wave_type,
         (double)freq_hz, (int)pkpk_uv);
    jint rc = (jint)ps2204a_set_siggen(dev, (ps_wave_t)wave_type, freq_hz,
                                       (uint32_t)pkpk_uv);
    if (rc != 0) {
        LOGE("ps2204a_set_siggen rc=%d (PS_ERROR_USB=-1, TIMEOUT=-3, STATE=-4, PARAM=-5)",
             (int)rc);
    } else {
        LOGI("ps2204a_set_siggen OK");
    }
    return rc;
}

/* Info ------------------------------------------------------------------- */

JNIEXPORT jstring JNICALL
JNI_FN(nativeGetSerial)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    char serial[32] = {0};
    ps2204a_get_info(dev, serial, sizeof(serial), NULL, 0);
    return (*env)->NewStringUTF(env, serial);
}

/* Resolution enhancement -------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetResolutionEnhancement)(JNIEnv *env, jclass cls, jlong handle,
                                       jint extra_bits)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_resolution_enhancement(dev, (int)extra_bits);
}

/* Overflow ---------------------------------------------------------------- */

/* [clipped_a, clipped_b, total] — the driver clamps corrected samples to
 * ±range, so these raw rail counts are the only way to tell a signal at full
 * scale from one driven past it. */
JNIEXPORT jintArray JNICALL
JNI_FN(nativeGetLastOverflow)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    ps_overflow_t ov = {0};
    if (ps2204a_get_last_overflow(dev, &ov) != PS_OK) return NULL;

    jintArray arr = (*env)->NewIntArray(env, 3);
    if (!arr) return NULL;
    jint vals[3] = { (jint)ov.clipped_a, (jint)ov.clipped_b, (jint)ov.total };
    (*env)->SetIntArrayRegion(env, arr, 0, 3, vals);
    return arr;
}

/* Equivalent-time sampling ------------------------------------------------ */

/* Returns the effective per-sample interval in picoseconds, or -status. */
JNIEXPORT jint JNICALL
JNI_FN(nativeSetEts)(JNIEnv *env, jclass cls, jlong handle,
                     jint mode, jint interleaves, jint cycles)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    int interval_ps = 0;
    ps_status_t st = ps2204a_set_ets(dev, (ps_ets_mode_t)mode,
                                     (int)interleaves, (int)cycles, &interval_ps);
    if (st != PS_OK) {
        LOGE("ps2204a_set_ets failed, status=%d", (int)st);
        return (jint)st;
    }
    return (jint)interval_ps;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeDisableEts)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_disable_ets(dev);
}

/* Interleaved [A..., B...] like nativeCaptureBlockDual. The per-sample
 * interval is whatever nativeSetEts reported. */
JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeCaptureEts)(JNIEnv *env, jclass cls, jlong handle, jint n_samples)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || n_samples <= 0 || n_samples > 8192) return NULL;

    /* Worst case is n_samples × 20 interleaves. */
    int cap = (int)n_samples * 20;
    float *a = (float *)malloc((size_t)cap * sizeof(float));
    float *b = (float *)malloc((size_t)cap * sizeof(float));
    if (!a || !b) { free(a); free(b); return NULL; }

    int actual = 0, interval_ps = 0;
    ps_status_t st = ps2204a_capture_ets(dev, (int)n_samples, a, b, cap,
                                         &actual, &interval_ps);
    if (st != PS_OK || actual <= 0) {
        LOGE("ps2204a_capture_ets failed, status=%d actual=%d", (int)st, actual);
        free(a); free(b);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, 2 * actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0, actual, a);
        (*env)->SetFloatArrayRegion(env, result, actual, actual, b);
    }
    free(a); free(b);
    return result;
}

/* Advanced triggers ------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetTriggerEx)(JNIEnv *env, jclass cls, jlong handle,
                           jint source, jfloat threshold_mv, jint dir,
                           jfloat delay_pct, jint auto_ms, jint hysteresis)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_trigger_ex(dev, (ps_channel_t)source,
                                        (float)threshold_mv,
                                        (ps_trigger_dir_t)dir,
                                        (float)delay_pct, (int)auto_ms,
                                        (int)hysteresis);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeSetTriggerWindow)(JNIEnv *env, jclass cls, jlong handle,
                               jint source, jfloat lower_mv, jfloat upper_mv,
                               jint dir, jfloat delay_pct, jint auto_ms)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_trigger_window(dev, (ps_channel_t)source,
                                            (float)lower_mv, (float)upper_mv,
                                            (ps_trigger_dir_t)dir,
                                            (float)delay_pct, (int)auto_ms);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeSetTriggerPwq)(JNIEnv *env, jclass cls, jlong handle,
                            jint source, jfloat threshold_mv, jint dir,
                            jint lower_ns, jint upper_ns,
                            jfloat delay_pct, jint auto_ms)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_trigger_pwq(dev, (ps_channel_t)source,
                                         (float)threshold_mv,
                                         (ps_trigger_dir_t)dir,
                                         (int)lower_ns, (int)upper_ns,
                                         (float)delay_pct, (int)auto_ms);
}

/* Arbitrary waveform ------------------------------------------------------ */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSiggenArbitrary)(JNIEnv *env, jclass cls, jlong handle,
                                 jshortArray lut, jfloat frequency_hz,
                                 jint pkpk_uv)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || !lut) return (jint)PS_ERROR_PARAM;

    jsize n = (*env)->GetArrayLength(env, lut);
    if (n < 2 || n > 4096) return (jint)PS_ERROR_PARAM;

    jshort *elems = (*env)->GetShortArrayElements(env, lut, NULL);
    if (!elems) return (jint)PS_ERROR_ALLOC;

    ps_status_t st = ps2204a_set_siggen_arbitrary(dev, (const int16_t *)elems,
                                                  (int)n, (float)frequency_hz,
                                                  (uint32_t)pkpk_uv);
    (*env)->ReleaseShortArrayElements(env, lut, elems, JNI_ABORT);
    return (jint)st;
}

/* Aggregated streaming ---------------------------------------------------- */

/* Returns [minA..., maxA..., minB..., maxB...], each `buckets` long, so the
 * caller can slice by a quarter. Aggregation is what keeps a narrow glitch
 * visible in a long window; plain decimation drops whatever falls between the
 * samples it keeps. */
JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeGetStreamingAggregated)(JNIEnv *env, jclass cls, jlong handle,
                                     jint n_buckets, jint span)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || n_buckets <= 0 || n_buckets > 65536) return NULL;

    size_t sz = (size_t)n_buckets * sizeof(float);
    float *mn_a = (float *)malloc(sz), *mx_a = (float *)malloc(sz);
    float *mn_b = (float *)malloc(sz), *mx_b = (float *)malloc(sz);
    if (!mn_a || !mx_a || !mn_b || !mx_b) {
        free(mn_a); free(mx_a); free(mn_b); free(mx_b);
        return NULL;
    }

    int actual = 0;
    ps_status_t st = ps2204a_get_streaming_aggregated(dev, mn_a, mx_a, mn_b, mx_b,
                                                      (int)n_buckets, (int)span,
                                                      &actual);
    if (st != PS_OK || actual <= 0) {
        free(mn_a); free(mx_a); free(mn_b); free(mx_b);
        return NULL;
    }

    jfloatArray result = (*env)->NewFloatArray(env, 4 * actual);
    if (result) {
        (*env)->SetFloatArrayRegion(env, result, 0 * actual, actual, mn_a);
        (*env)->SetFloatArrayRegion(env, result, 1 * actual, actual, mx_a);
        (*env)->SetFloatArrayRegion(env, result, 2 * actual, actual, mn_b);
        (*env)->SetFloatArrayRegion(env, result, 3 * actual, actual, mx_b);
    }
    free(mn_a); free(mx_a); free(mn_b); free(mx_b);
    return result;
}

/* EEPROM calibration ------------------------------------------------------ */

/* Returns [valid, active, offset_mv × 9, gainA × 9, gainB × 9] = 29 floats.
 * See docs/protocol.md: the offsets are confirmed, the gain blocks are not,
 * which is why applying this table is opt-in. */
JNIEXPORT jfloatArray JNICALL
JNI_FN(nativeGetEepromCalibration)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    ps_cal_table_t cal;
    if (ps2204a_get_eeprom_calibration(dev, &cal) != PS_OK) return NULL;

    const int n = PS2204A_NUM_RANGES;
    jfloatArray arr = (*env)->NewFloatArray(env, 2 + 3 * n);
    if (!arr) return NULL;

    float head[2] = {
        cal.valid ? 1.0f : 0.0f,
        ps2204a_eeprom_calibration_active(dev) ? 1.0f : 0.0f,
    };
    (*env)->SetFloatArrayRegion(env, arr, 0, 2, head);
    (*env)->SetFloatArrayRegion(env, arr, 2,         n, cal.offset_mv);
    (*env)->SetFloatArrayRegion(env, arr, 2 + n,     n, cal.gain[0]);
    (*env)->SetFloatArrayRegion(env, arr, 2 + 2 * n, n, cal.gain[1]);
    return arr;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeUseEepromCalibration)(JNIEnv *env, jclass cls, jlong handle,
                                   jboolean enable)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_use_eeprom_calibration(dev, enable == JNI_TRUE);
}

/* ======================================================================
 * Remaining driver surface
 *
 * These were the functions the shim did not expose. Three of the driver's
 * public functions stay out on purpose: ps2204a_open() (Android always comes
 * in through the file-descriptor path, nativeOpen / nativeOpenStage1),
 * ps2204a_debug_capture_cmds() (a test hook for asserting byte patterns), and
 * ps2204a_set_siggen_raw() (a pre-encoded frequency word, which
 * nativeSetSiggenEx covers in engineering units).
 * ====================================================================== */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSiggenEx)(JNIEnv *env, jclass cls, jlong handle,
                          jint wave_type, jfloat start_hz, jfloat stop_hz,
                          jfloat increment_hz, jfloat dwell_s,
                          jint pkpk_uv, jint offset_uv, jint duty_pct)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (duty_pct < 0)   duty_pct = 0;
    if (duty_pct > 100) duty_pct = 100;
    return (jint)ps2204a_set_siggen_ex(dev, (ps_wave_t)wave_type,
                                       (float)start_hz, (float)stop_hz,
                                       (float)increment_hz, (float)dwell_s,
                                       (uint32_t)pkpk_uv, (int32_t)offset_uv,
                                       (uint8_t)duty_pct);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeDisableSiggen)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    return (jint)ps2204a_disable_siggen((ps2204a_device_t *)(intptr_t)handle);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeCalibrateDcOffset)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    return (jint)ps2204a_calibrate_dc_offset((ps2204a_device_t *)(intptr_t)handle);
}

JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeGetEepromRaw)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    uint8_t raw[256];
    if (ps2204a_get_eeprom_raw(dev, raw, (int)sizeof(raw)) != PS_OK) return NULL;
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)sizeof(raw));
    if (!arr) return NULL;
    (*env)->SetByteArrayRegion(env, arr, 0, (jsize)sizeof(raw), (const jbyte *)raw);
    return arr;
}

JNIEXPORT jbyteArray JNICALL
JNI_FN(nativeCaptureRaw)(JNIEnv *env, jclass cls, jlong handle, jint samples)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (samples <= 0 || samples > 16384) return NULL;
    /* Dual-channel blocks interleave, so the byte count can reach 2x. */
    int cap = samples * 2;
    uint8_t *raw = (uint8_t *)malloc((size_t)cap);
    if (!raw) return NULL;
    int got = 0;
    if (ps2204a_capture_raw(dev, samples, raw, cap, &got) != PS_OK || got <= 0) {
        free(raw);
        return NULL;
    }
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)got);
    if (arr) (*env)->SetByteArrayRegion(env, arr, 0, (jsize)got, (const jbyte *)raw);
    free(raw);
    return arr;
}

JNIEXPORT jboolean JNICALL
JNI_FN(nativeIsStreaming)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return ps2204a_is_streaming(dev) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
JNI_FN(nativeMaxSamples)(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env; (void)cls;
    return (jint)ps2204a_max_samples((ps2204a_device_t *)(intptr_t)handle);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSdkStreamIntervalNs)(JNIEnv *env, jclass cls, jlong handle,
                                     jint interval_ns)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (interval_ns < 0) interval_ns = 0;
    return (jint)ps2204a_set_sdk_stream_interval_ns(dev, (uint32_t)interval_ns);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSdkStreamAutoStop)(JNIEnv *env, jclass cls, jlong handle,
                                   jlong max_samples)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (max_samples < 0) max_samples = 0;
    return (jint)ps2204a_set_sdk_stream_auto_stop(dev, (uint64_t)max_samples);
}

JNIEXPORT jint JNICALL
JNI_FN(nativeTimebaseToNs)(JNIEnv *env, jclass cls, jint timebase)
{
    (void)env; (void)cls;
    return (jint)ps2204a_timebase_to_ns((int)timebase);
}
