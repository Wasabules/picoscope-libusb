/*
 * JNI wrapper over the reverse-engineered PicoScope 2204A libusb driver.
 * Consumed by io.github.wasabules.ps2204.PicoScope2204A.
 */

#include <jni.h>
#include <stdlib.h>
#include "picoscope2204a.h"

#define JNI_FN(name) Java_io_github_wasabules_ps2204_PicoScope2204A_##name

/* Open / close ----------------------------------------------------------- */

JNIEXPORT jlong JNICALL
JNI_FN(nativeOpen)(JNIEnv *env, jclass cls, jint usb_fd)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open_with_fd(&dev, (int)usb_fd);
    if (st != PS_OK) return 0;
    return (jlong)(intptr_t)dev;
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

/* Streaming -------------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeStartStreaming)(JNIEnv *env, jclass cls, jlong handle, jint interval_us)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_start_streaming(dev, (int)interval_us, NULL, NULL, 0);
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

/* Signal generator ------------------------------------------------------- */

JNIEXPORT jint JNICALL
JNI_FN(nativeSetSiggen)(JNIEnv *env, jclass cls, jlong handle,
                       jint wave_type, jfloat freq_hz, jint pkpk_uv)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_siggen(dev, (ps_wave_t)wave_type, freq_hz,
                                    (uint32_t)pkpk_uv);
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
