/*
 * PicoScope 2204A JNI wrapper for Android
 *
 * Usage from Java/Kotlin:
 *   System.loadLibrary("picoscope_jni");
 *   long handle = PicoScope2204A.nativeOpen(usbFd);
 *   PicoScope2204A.nativeSetChannel(handle, 0, true, 1, 8);
 *   float[] data = PicoScope2204A.nativeCaptureBlock(handle, 1000);
 *   PicoScope2204A.nativeClose(handle);
 */

#include <jni.h>
#include "../picoscope2204a.h"
#include <stdlib.h>

#define JNI_CLASS "com/picoscope/PicoScope2204A"

/* ======================================================================== */
/* Open / Close                                                             */
/* ======================================================================== */

JNIEXPORT jlong JNICALL
Java_com_picoscope_PicoScope2204A_nativeOpen(JNIEnv *env, jclass cls,
                                              jint usb_fd)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = NULL;
    ps_status_t st = ps2204a_open_with_fd(&dev, (int)usb_fd);
    if (st != PS_OK) return 0;
    return (jlong)(intptr_t)dev;
}

JNIEXPORT void JNICALL
Java_com_picoscope_PicoScope2204A_nativeClose(JNIEnv *env, jclass cls,
                                               jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (dev) ps2204a_close(dev);
}

/* ======================================================================== */
/* Configuration                                                            */
/* ======================================================================== */

JNIEXPORT jint JNICALL
Java_com_picoscope_PicoScope2204A_nativeSetChannel(JNIEnv *env, jclass cls,
                                                    jlong handle,
                                                    jint channel,
                                                    jboolean enabled,
                                                    jint coupling,
                                                    jint range)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_channel(dev, (ps_channel_t)channel,
                                     (bool)enabled,
                                     (ps_coupling_t)coupling,
                                     (ps_range_t)range);
}

JNIEXPORT jint JNICALL
Java_com_picoscope_PicoScope2204A_nativeSetTimebase(JNIEnv *env, jclass cls,
                                                     jlong handle,
                                                     jint timebase,
                                                     jint samples)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_timebase(dev, (int)timebase, (int)samples);
}

/* ======================================================================== */
/* Block Capture                                                            */
/* ======================================================================== */

JNIEXPORT jfloatArray JNICALL
Java_com_picoscope_PicoScope2204A_nativeCaptureBlock(JNIEnv *env, jclass cls,
                                                      jlong handle,
                                                      jint samples)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev) return NULL;

    float *buf = (float *)malloc(samples * sizeof(float));
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

/* ======================================================================== */
/* Streaming                                                                */
/* ======================================================================== */

JNIEXPORT jint JNICALL
Java_com_picoscope_PicoScope2204A_nativeStartStreaming(JNIEnv *env,
                                                        jclass cls,
                                                        jlong handle,
                                                        jint interval_us)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_start_streaming(dev, (int)interval_us,
                                          NULL, NULL, 0);
}

JNIEXPORT jint JNICALL
Java_com_picoscope_PicoScope2204A_nativeStopStreaming(JNIEnv *env,
                                                       jclass cls,
                                                       jlong handle)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_stop_streaming(dev);
}

JNIEXPORT jfloatArray JNICALL
Java_com_picoscope_PicoScope2204A_nativeGetLatest(JNIEnv *env, jclass cls,
                                                    jlong handle, jint n)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    if (!dev || n <= 0) return NULL;

    float *buf = (float *)malloc(n * sizeof(float));
    if (!buf) return NULL;

    int actual = 0;
    ps_status_t st = ps2204a_get_streaming_latest(dev, buf, NULL, (int)n,
                                                   &actual);
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

/* ======================================================================== */
/* Signal Generator                                                         */
/* ======================================================================== */

JNIEXPORT jint JNICALL
Java_com_picoscope_PicoScope2204A_nativeSetSiggen(JNIEnv *env, jclass cls,
                                                    jlong handle,
                                                    jint wave_type,
                                                    jfloat freq_hz)
{
    (void)env; (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    return (jint)ps2204a_set_siggen(dev, (ps_wave_t)wave_type, freq_hz);
}

/* ======================================================================== */
/* Info                                                                     */
/* ======================================================================== */

JNIEXPORT jstring JNICALL
Java_com_picoscope_PicoScope2204A_nativeGetSerial(JNIEnv *env, jclass cls,
                                                    jlong handle)
{
    (void)cls;
    ps2204a_device_t *dev = (ps2204a_device_t *)(intptr_t)handle;
    char serial[32] = {0};
    ps2204a_get_info(dev, serial, sizeof(serial), NULL, 0);
    return (*env)->NewStringUTF(env, serial);
}
