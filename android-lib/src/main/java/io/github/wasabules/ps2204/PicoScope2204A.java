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

    /**
     * Android two-phase open — stage 1.
     *
     * <p>The FX2 firmware upload re-enumerates the USB device. On
     * Android the original file descriptor is invalidated and the app
     * cannot rescan /dev/bus/usb, so the caller must drive the
     * re-enumeration themselves:</p>
     *
     * <ol>
     *   <li>Call {@code nativeOpenStage1(fd)} with the fd obtained from
     *       the first {@link android.hardware.usb.UsbDeviceConnection}.</li>
     *   <li>Close that {@code UsbDeviceConnection}. Android emits
     *       {@code USB_DEVICE_DETACHED} and shortly afterwards
     *       {@code USB_DEVICE_ATTACHED} for the post-renum device.</li>
     *   <li>Obtain USB permission + a new {@code UsbDeviceConnection}
     *       for that device and call {@link #nativeOpenStage2(long, int)}
     *       with its fd.</li>
     * </ol>
     *
     * <p>On any stage2 failure the caller must still call
     * {@link #nativeClose(long)} to reclaim the handle produced here.</p>
     *
     * @return opaque device handle, or 0 on failure
     */
    public static native long nativeOpenStage1(int usbFd);

    /** @return 0 on success, negative {@code ps_status_t} otherwise */
    public static native int nativeOpenStage2(long handle, int newUsbFd);

    public static native void nativeClose(long handle);

    public static native int nativeSetChannel(long handle, int channel,
                                              boolean enabled, int coupling,
                                              int range);

    public static native int nativeSetTimebase(long handle, int timebase,
                                               int samples);

    /** @return sample buffer in mV, or {@code null} on failure */
    public static native float[] nativeCaptureBlock(long handle, int samples);

    /**
     * Dual-channel block capture. Returns a flat array of length {@code 2*actual}
     * where the first {@code actual} floats are CH A and the next {@code actual}
     * are CH B. Callers slice based on which channels they enabled. Returns
     * {@code null} on failure.
     */
    public static native float[] nativeCaptureBlockDual(long handle, int samples);

    /* Trigger directions — match ps_trigger_dir_t in the C driver. */
    public static final int TRIGGER_RISING  = 0;
    public static final int TRIGGER_FALLING = 1;

    /**
     * Configure a level trigger.
     * @param source        {@link #CHANNEL_A} or {@link #CHANNEL_B}
     * @param thresholdMv   trigger voltage in mV (within the source's range)
     * @param direction     {@link #TRIGGER_RISING} or {@link #TRIGGER_FALLING}
     * @param delayPct      trigger position, -100..+100 %
     *                      (-100 = all pre-trigger, 0 = centred, +100 = all post)
     * @param autoTriggerMs host-side timeout; 0 = normal (wait forever),
     *                      &gt;0 = auto-fire after this delay if no edge seen
     * @return 0 on success, negative error code otherwise
     */
    public static native int nativeSetTrigger(long handle, int source,
                                              float thresholdMv, int direction,
                                              float delayPct, int autoTriggerMs);

    /** Revert to free-running (auto-trigger immediately). */
    public static native int nativeDisableTrigger(long handle);

    /**
     * Push a (offset_mv, gain) correction for a given range. Captured samples
     * are post-processed as {@code out = (raw − offset_mv) × gain}.
     * @return 0 on success, negative error code otherwise
     */
    public static native int nativeSetRangeCalibration(long handle, int range,
                                                       float offsetMv, float gain);

    /**
     * Read the current (offset_mv, gain) for a range.
     * @return {@code float[]{offset_mv, gain}} or {@code null} on failure.
     */
    public static native float[] nativeGetRangeCalibration(long handle, int range);

    /* Streaming modes — must match ps_stream_mode_t in the C driver. */
    public static final int STREAM_FAST   = 0;
    public static final int STREAM_NATIVE = 1;
    public static final int STREAM_SDK    = 2;

    public static native int nativeStartStreaming(long handle, int intervalUs);

    /**
     * Start streaming with explicit mode selection.
     * @param mode one of {@link #STREAM_FAST}, {@link #STREAM_NATIVE}, {@link #STREAM_SDK}
     */
    public static native int nativeStartStreamingMode(long handle, int mode,
                                                      int intervalUs);

    public static native int nativeStopStreaming(long handle);

    /** Per-sample time in ns for the currently-running stream (mode-dependent). */
    public static native int nativeGetStreamingDtNs(long handle);

    /**
     * Streaming statistics snapshot.
     * Returns {@code [blocks, total_samples, elapsed_s, samples_per_sec,
     *                  blocks_per_sec, last_block_ms]} or {@code null}.
     */
    public static native double[] nativeGetStreamingStats(long handle);

    public static native float[] nativeGetLatest(long handle, int n);

    /**
     * Dual-channel latest fetch. Returns a flat array of length {@code 2*actual}
     * where the first {@code actual} floats are CH A and the next {@code actual}
     * are CH B. {@code null} on failure.
     */
    public static native float[] nativeGetLatestDual(long handle, int n);

    public static native int nativeSetSiggen(long handle, int waveType,
                                             float freqHz, int pkpkUv);

    /** @return device serial in the form {@code JOxxxxxxxx}, or empty string */
    public static native String nativeGetSerial(long handle);

    /* ====================================================================
     * Resolution enhancement
     * ==================================================================== */

    /**
     * Trade bandwidth for vertical resolution by averaging 4^extraBits
     * neighbouring samples. Each step halves the noise: measured on this
     * hardware, 6.14 effective bits at 0 rises to 9.44 at 4.
     *
     * @param extraBits 0 (off) to 4
     * @return 0 on success, negative {@code PS_ERROR_*} otherwise
     */
    public static native int nativeSetResolutionEnhancement(long handle, int extraBits);

    /* ====================================================================
     * Overflow
     * ==================================================================== */

    /**
     * Rail counts for the most recent capture, as
     * {@code [clippedA, clippedB, total]}, or {@code null} on failure.
     *
     * <p>The driver clamps corrected samples to ±range so a gain above 1
     * cannot extrapolate past the rails — which also hides the difference
     * between a signal sitting at full scale and one driven beyond it. These
     * counts come from the raw ADC codes, before scaling, so a non-zero value
     * means the input genuinely exceeded the selected range.</p>
     */
    public static native int[] nativeGetLastOverflow(long handle);

    /* ====================================================================
     * Equivalent-time sampling
     * ==================================================================== */

    /** ETS off. */
    public static final int ETS_OFF  = 0;
    /** ETS ≈1 GS/s effective (10 interleaves, 2 cycles by default). */
    public static final int ETS_FAST = 1;
    /** ETS ≈2 GS/s effective (20 interleaves, 4 cycles by default). */
    public static final int ETS_SLOW = 2;

    /**
     * Enable equivalent-time sampling. The ADC runs at 100 MS/s; on a
     * repetitive signal the device captures many triggered blocks, each offset
     * by a fraction of a sample period, and interleaves them. Requires a stable
     * trigger and a waveform that actually repeats — a one-shot event yields
     * noise.
     *
     * @param mode        {@link #ETS_OFF}, {@link #ETS_FAST} or {@link #ETS_SLOW}
     * @param interleaves 2..20, or 0 for the mode default
     * @param cycles      1..32, or 0 for the mode default
     * @return effective per-sample interval in picoseconds, or a negative
     *         {@code PS_ERROR_*} on failure
     */
    public static native int nativeSetEts(long handle, int mode,
                                          int interleaves, int cycles);

    /** Disable ETS and resume normal block capture. */
    public static native int nativeDisableEts(long handle);

    /**
     * Run an ETS acquisition. Returns a flat array of length {@code 2*actual}:
     * the first {@code actual} floats are CH A, the next {@code actual} are
     * CH B — the same layout as {@link #nativeCaptureBlockDual}. The per-sample
     * interval is whatever {@link #nativeSetEts} reported, not the timebase.
     *
     * @param nSamples per-cycle sample count, 1..8192; the result holds up to
     *                 {@code nSamples × interleaves} points
     */
    public static native float[] nativeCaptureEts(long handle, int nSamples);

    /* ====================================================================
     * Advanced triggers
     * ==================================================================== */

    /**
     * Edge trigger with a hysteresis band, in ADC codes, that stops a noisy
     * edge from re-triggering on its own ripple.
     */
    public static native int nativeSetTriggerEx(long handle, int source,
                                                float thresholdMv, int dir,
                                                float delayPct, int autoMs,
                                                int hysteresisCounts);

    /**
     * Fire when the signal enters ({@code dir = RISING}) or leaves
     * ({@code dir = FALLING}) the band between {@code lowerMv} and
     * {@code upperMv} — the usual way to catch a level drifting out of
     * tolerance.
     */
    public static native int nativeSetTriggerWindow(long handle, int source,
                                                    float lowerMv, float upperMv,
                                                    int dir, float delayPct,
                                                    int autoMs);

    /**
     * Fire on a level crossing only when the pulse that preceded it lasted
     * between {@code lowerNs} and {@code upperNs} — for hunting runts and
     * glitches. Pass {@code upperNs = 0} for no upper bound.
     */
    public static native int nativeSetTriggerPwq(long handle, int source,
                                                 float thresholdMv, int dir,
                                                 int lowerNs, int upperNs,
                                                 float delayPct, int autoMs);

    /* ====================================================================
     * Arbitrary waveform
     * ==================================================================== */

    /**
     * Upload a user-defined waveform. The device is a true AWG — every siggen
     * call already uploads an 8192-byte LUT — so the built-in shapes are not a
     * hardware limit.
     *
     * @param lut         2..4096 samples, full-scale {@code -32768..32767};
     *                    shorter inputs are linearly resampled
     * @param frequencyHz playback frequency
     * @param pkpkUv      peak-to-peak amplitude in microvolts
     */
    public static native int nativeSetSiggenArbitrary(long handle, short[] lut,
                                                      float frequencyHz, int pkpkUv);

    /* ====================================================================
     * Aggregated streaming
     * ==================================================================== */

    /**
     * Reduce the streaming ring to {@code nBuckets} (min, max) pairs. Returns a
     * flat array of length {@code 4*actual}: {@code minA}, {@code maxA},
     * {@code minB}, {@code maxB}, each {@code actual} long — so slice it in
     * quarters. {@code null} on failure.
     *
     * <p>Decimating instead drops whatever falls between the samples it keeps,
     * so a narrow glitch in a multi-second window simply disappears. Keeping
     * both extremes of each bucket bounds the signal rather than sampling it,
     * which is what a roll display needs once the window is longer than the
     * pixel count.</p>
     *
     * @param span number of trailing samples to cover, or 0 for everything the
     *             ring holds
     */
    public static native float[] nativeGetStreamingAggregated(long handle,
                                                              int nBuckets,
                                                              int span);

    /* ====================================================================
     * EEPROM calibration
     * ==================================================================== */

    /**
     * Per-unit factory trim decoded from the device's own EEPROM. Returns 29
     * floats: {@code [valid, active, offsetMv × 9, gainA × 9, gainB × 9]},
     * ranges ordered 50 mV … 20 V. {@code null} on failure.
     *
     * <p>The offsets are confirmed against a hand-measured reference (Pearson
     * r = 0.9989). The gain blocks are not: measuring one signal across four
     * ranges — which the correct table must render identically — came out worse
     * with them than with the built-in table, so the driver still applies the
     * built-in one by default. See {@code docs/protocol.md}.</p>
     */
    public static native float[] nativeGetEepromCalibration(long handle);

    /**
     * Switch between the device's own trim and the built-in reference table.
     * Fails with {@code PS_ERROR_STATE} if the EEPROM did not decode.
     */
    public static native int nativeUseEepromCalibration(long handle, boolean enable);
}
