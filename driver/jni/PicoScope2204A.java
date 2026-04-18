package com.picoscope;

/**
 * PicoScope 2204A Android wrapper.
 *
 * Usage:
 *   UsbDeviceConnection conn = usbManager.openDevice(device);
 *   int fd = conn.getFileDescriptor();
 *   long handle = PicoScope2204A.nativeOpen(fd);
 *   PicoScope2204A.nativeSetChannel(handle, 0, true, 1, 8); // CH_A, DC, 5V
 *   PicoScope2204A.nativeSetTimebase(handle, 5, 1000);
 *   float[] data = PicoScope2204A.nativeCaptureBlock(handle, 1000);
 *   PicoScope2204A.nativeClose(handle);
 */
public class PicoScope2204A {

    static {
        System.loadLibrary("picoscope_jni");
    }

    /* Channels */
    public static final int CHANNEL_A = 0;
    public static final int CHANNEL_B = 1;

    /* Coupling */
    public static final int AC = 0;
    public static final int DC = 1;

    /* Ranges (match ps_range_t enum) */
    public static final int RANGE_50MV  = 2;
    public static final int RANGE_100MV = 3;
    public static final int RANGE_200MV = 4;
    public static final int RANGE_500MV = 5;
    public static final int RANGE_1V    = 6;
    public static final int RANGE_2V    = 7;
    public static final int RANGE_5V    = 8;
    public static final int RANGE_10V   = 9;
    public static final int RANGE_20V   = 10;

    /* Wave types */
    public static final int WAVE_SINE     = 0;
    public static final int WAVE_SQUARE   = 1;
    public static final int WAVE_TRIANGLE = 2;
    public static final int WAVE_RAMPUP   = 3;
    public static final int WAVE_RAMPDOWN = 4;
    public static final int WAVE_DC       = 5;

    /* Native methods */
    public static native long nativeOpen(int usbFd);
    public static native void nativeClose(long handle);

    public static native int nativeSetChannel(long handle, int channel,
                                              boolean enabled, int coupling,
                                              int range);
    public static native int nativeSetTimebase(long handle, int timebase,
                                               int samples);

    public static native float[] nativeCaptureBlock(long handle, int samples);

    public static native int nativeStartStreaming(long handle, int intervalUs);
    public static native int nativeStopStreaming(long handle);
    public static native float[] nativeGetLatest(long handle, int n);

    public static native int nativeSetSiggen(long handle, int waveType,
                                             float freqHz);
    public static native String nativeGetSerial(long handle);
}
