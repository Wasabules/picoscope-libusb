package io.github.wasabules.ps2204;

/**
 * Android JNI bindings for the reverse-engineered PicoScope 2204A libusb
 * driver. Backed by {@code libpicoscope_jni.so}.
 *
 * <p>Typical flow — the app must have already received USB permission
 * and held onto a {@link android.hardware.usb.UsbDeviceConnection}:</p>
 *
 * <pre>{@code
 *   int fd = connection.getFileDescriptor();
 *   long handle = PicoScope2204A.nativeOpen(fd);
 *   PicoScope2204A.nativeSetChannel(handle,
 *       PicoScope2204A.CHANNEL_A, true,
 *       PicoScope2204A.DC, PicoScope2204A.RANGE_5V);
 *   PicoScope2204A.nativeSetTimebase(handle, 5, 1000);
 *   float[] samples = PicoScope2204A.nativeCaptureBlock(handle, 1000);
 *   PicoScope2204A.nativeClose(handle);
 * }</pre>
 *
 * <p>All native methods are thread-safe with respect to different
 * handles. Do not call native methods for the same handle from
 * multiple threads without external synchronisation — the underlying
 * C driver uses a single libusb context per device.</p>
 *
 * <p>This project is not affiliated with Pico Technology Ltd.</p>
 */
public final class PicoScope2204A {

    static {
        System.loadLibrary("picoscope_jni");
    }

    private PicoScope2204A() { /* static utility class */ }

    /* Channels */
    public static final int CHANNEL_A = 0;
    public static final int CHANNEL_B = 1;

    /* Coupling */
    public static final int AC = 0;
    public static final int DC = 1;

    /* Ranges — enum values match ps_range_t in the C driver */
    public static final int RANGE_50MV  = 2;
    public static final int RANGE_100MV = 3;
    public static final int RANGE_200MV = 4;
    public static final int RANGE_500MV = 5;
    public static final int RANGE_1V    = 6;
    public static final int RANGE_2V    = 7;
    public static final int RANGE_5V    = 8;
    public static final int RANGE_10V   = 9;
    public static final int RANGE_20V   = 10;

    /* Wave types — match ps_wave_t in the C driver */
    public static final int WAVE_SINE     = 0;
    public static final int WAVE_SQUARE   = 1;
    public static final int WAVE_TRIANGLE = 2;
    public static final int WAVE_RAMPUP   = 3;
    public static final int WAVE_RAMPDOWN = 4;
    public static final int WAVE_DC       = 5;

    /**
     * Open the device given a USB file descriptor from
     * {@link android.hardware.usb.UsbDeviceConnection#getFileDescriptor()}.
     *
     * @return opaque device handle, or 0 on failure
     */
    /**
     * Tell the native driver where to find the four firmware blobs
     * ({@code fx2.bin}, {@code fpga.bin}, {@code stream_lut.bin},
     * {@code waveform.bin}). Must be called before {@link #nativeOpen(int)}
     * on Android — the driver otherwise searches Linux-desktop paths that
     * app sandboxes can't read.
     *
     * @param path absolute path to a readable directory
     * @return 0 on success, -1 on failure (e.g. null path)
     */
    public static native int nativeSetFirmwareDir(String path);

    public static native long nativeOpen(int usbFd);

    public static native void nativeClose(long handle);

    public static native int nativeSetChannel(long handle, int channel,
                                              boolean enabled, int coupling,
                                              int range);

    public static native int nativeSetTimebase(long handle, int timebase,
                                               int samples);

    /** @return sample buffer in mV, or {@code null} on failure */
    public static native float[] nativeCaptureBlock(long handle, int samples);

    public static native int nativeStartStreaming(long handle, int intervalUs);

    public static native int nativeStopStreaming(long handle);

    public static native float[] nativeGetLatest(long handle, int n);

    public static native int nativeSetSiggen(long handle, int waveType,
                                             float freqHz, int pkpkUv);

    /** @return device serial in the form {@code JOxxxxxxxx}, or empty string */
    public static native String nativeGetSerial(long handle);
}
