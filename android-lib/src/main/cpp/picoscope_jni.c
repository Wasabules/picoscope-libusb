/*
 * JNI wrapper over the reverse-engineered PicoScope 2204A libusb driver.
 * Consumed by io.github.wasabules.ps2204.PicoScope2204A.
 */

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
