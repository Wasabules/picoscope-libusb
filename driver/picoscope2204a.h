/*
 * PicoScope 2204A libusb driver
 * High-performance C driver for Linux and Android (NDK/JNI)
 *
 * Reverse-engineered USB protocol — no proprietary SDK required.
 * Hardware: Cypress FX2 (CY7C68013A) + Xilinx FPGA, 8-bit ADC
 */
#ifndef PICOSCOPE2204A_H
#define PICOSCOPE2204A_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Types
 * ======================================================================== */

typedef struct ps2204a_device ps2204a_device_t;

typedef enum {
    PS_CHANNEL_A = 0,
    PS_CHANNEL_B = 1
} ps_channel_t;

typedef enum {
    PS_AC = 0,
    PS_DC = 1
} ps_coupling_t;

/* Voltage ranges — values match PicoSDK ps2000 enum (R_50MV=2 .. R_20V=10).
 * PS2204A supports ranges 2-10 (50 mV to 20 V). */
typedef enum {
    PS_50MV  = 2,
    PS_100MV = 3,
    PS_200MV = 4,
    PS_500MV = 5,
    PS_1V    = 6,
    PS_2V    = 7,
    PS_5V    = 8,
    PS_10V   = 9,
    PS_20V   = 10
} ps_range_t;

typedef enum {
    PS_RISING  = 0,
    PS_FALLING = 1
} ps_trigger_dir_t;

/* Trigger mode (advanced-trigger subset reverse-engineered from the SDK).
 * PS_TRIGGER_LEVEL is what every basic set_trigger call has used so far; the
 * other modes are on top of the same compound-command protocol. */
typedef enum {
    PS_TRIGGER_LEVEL  = 0,   /* Threshold + hysteresis on a single edge */
    PS_TRIGGER_WINDOW = 1,   /* Enter/exit a [lo, hi] voltage window */
    PS_TRIGGER_PWQ    = 2    /* Pulse-width qualifier on a LEVEL trigger */
} ps_trigger_mode_t;

typedef enum {
    PS_WAVE_SINE     = 0,
    PS_WAVE_SQUARE   = 1,
    PS_WAVE_TRIANGLE = 2,
    PS_WAVE_RAMPUP   = 3,
    PS_WAVE_RAMPDOWN = 4,
    PS_WAVE_DC       = 5
} ps_wave_t;

typedef enum {
    PS_OK            =  0,
    PS_ERROR_USB     = -1,
    PS_ERROR_FW      = -2,
    PS_ERROR_TIMEOUT = -3,
    PS_ERROR_STATE   = -4,
    PS_ERROR_PARAM   = -5,
    PS_ERROR_ALLOC   = -6
} ps_status_t;

/* Streaming callback — called from streaming thread for each block */
typedef void (*ps_stream_cb_t)(const float *data_a, const float *data_b,
                               int n_samples, void *user_data);

/* Streaming mode selection */
typedef enum {
    PS_STREAM_FAST   = 0,  /* Rapid block capture, ~330+ kS/s */
    PS_STREAM_NATIVE = 1,  /* FPGA continuous mode, ~100 S/s (hardware-limited) */
    PS_STREAM_SDK    = 2   /* SDK-style continuous 1 MS/s dual-channel (gap-free) */
} ps_stream_mode_t;

/* Streaming statistics */
typedef struct {
    uint64_t blocks;
    uint64_t total_samples;
    double   elapsed_s;
    double   samples_per_sec;
    double   blocks_per_sec;
    double   last_block_ms;
} ps_stream_stats_t;

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

/* Open PicoScope 2204A — full init (FX2 firmware + ADC + FPGA + channels).
 * Allocates device handle on success. Caller must call ps2204a_close(). */
ps_status_t ps2204a_open(ps2204a_device_t **dev);

/* Open with an existing USB file descriptor (Android UsbManager).
 * The fd must be obtained via UsbDeviceConnection.getFileDescriptor(). */
ps_status_t ps2204a_open_with_fd(ps2204a_device_t **dev, int usb_fd);

/* Two-phase Android open.
 *
 * On Android, the FX2 firmware upload re-enumerates the USB device.
 * The file descriptor we were handed via UsbDeviceConnection is then
 * invalidated by the kernel, and Android's sandbox forbids scanning
 * /dev/bus/usb to find the post-renum device. So the app must:
 *
 *   1. Call ps2204a_open_fd_stage1() with the initial fd. This loads
 *      firmware blobs, uploads FX2, then releases the USB handle so
 *      Android's UsbDeviceConnection can be cleanly closed.
 *   2. Wait for the USB re-attach event (same VID/PID), request USB
 *      permission, and obtain a new file descriptor.
 *   3. Call ps2204a_open_fd_stage2() with the new fd. This resumes init
 *      (ADC, FPGA, channel setup) and leaves the device fully open.
 *
 * On failure in stage2, the caller must still call ps2204a_close()
 * to reclaim the allocation produced by stage1.
 */
ps_status_t ps2204a_open_fd_stage1(ps2204a_device_t **dev, int usb_fd);
ps_status_t ps2204a_open_fd_stage2(ps2204a_device_t *dev, int new_usb_fd);

/* Close device and free all resources. */
void ps2204a_close(ps2204a_device_t *dev);

/* ========================================================================
 * Configuration
 * ======================================================================== */

/* Configure a channel. Settings are applied at capture time via gain bytes. */
ps_status_t ps2204a_set_channel(ps2204a_device_t *dev, ps_channel_t ch,
                                bool enabled, ps_coupling_t coupling,
                                ps_range_t range);

/* Set timebase: tb=0 → 10ns (100 MSPS), tb=1 → 20ns, etc.
 * Formula: interval_ns = 10 * 2^timebase. Max: 23. */
ps_status_t ps2204a_set_timebase(ps2204a_device_t *dev, int timebase,
                                 int samples);

/* Configure trigger.
 *   source:        PS_CHANNEL_A or PS_CHANNEL_B
 *   threshold_mv:  trigger voltage in mV (within the chosen range)
 *   dir:           PS_RISING or PS_FALLING
 *   delay_pct:     trigger position in the captured block, −100..+100 %
 *                   −100 = trigger at end (all pre-trigger)
 *                     0  = trigger centred (half pre, half post)
 *                   +100 = trigger at start (all post-trigger)
 *   auto_trigger_ms: host-side timeout; if trigger doesn't fire within this
 *                    many ms the driver falls back to a free-run capture. */
ps_status_t ps2204a_set_trigger(ps2204a_device_t *dev, ps_channel_t source,
                                float threshold_mv, ps_trigger_dir_t dir,
                                float delay_pct, int auto_trigger_ms);

/* Extended trigger with explicit hysteresis (ADC counts, default 10). */
ps_status_t ps2204a_set_trigger_ex(ps2204a_device_t *dev, ps_channel_t source,
                                   float threshold_mv, ps_trigger_dir_t dir,
                                   float delay_pct, int auto_trigger_ms,
                                   int hysteresis_counts);

/* Disable trigger (auto-trigger / free-running mode). */
ps_status_t ps2204a_disable_trigger(ps2204a_device_t *dev);

/* Window trigger: fire when the signal enters [lower_mv, upper_mv] (dir
 * PS_RISING) or exits it (PS_FALLING). Uses the same source/channel and
 * delay_pct semantics as ps2204a_set_trigger. */
ps_status_t ps2204a_set_trigger_window(ps2204a_device_t *dev,
                                       ps_channel_t source,
                                       float lower_mv, float upper_mv,
                                       ps_trigger_dir_t dir,
                                       float delay_pct, int auto_trigger_ms);

/* Pulse-Width Qualifier: fire on a level trigger only when the preceding
 * pulse matches the width constraint. lower_ns / upper_ns bound the pulse
 * duration; direction specifies which edge starts the pulse. */
ps_status_t ps2204a_set_trigger_pwq(ps2204a_device_t *dev,
                                    ps_channel_t source,
                                    float threshold_mv, ps_trigger_dir_t dir,
                                    int lower_ns, int upper_ns,
                                    float delay_pct, int auto_trigger_ms);

/* ========================================================================
 * Enhanced Resolution (software oversampling)
 * ======================================================================== */

/* Enable software oversampling for enhanced vertical resolution.
 *   extra_bits: 0..4. Applies a moving-average box filter of N=4^extra_bits
 *   samples to every capture and every streaming block. N=1 (0 extra bits)
 *   disables the filter; N=256 (4 extra bits) turns the native 8-bit ADC
 *   into an effective 12-bit readout at the cost of signal bandwidth
 *   (−3 dB at ≈ fs / (π·N)).
 *
 * The filter is applied to the scaled float output (mV), so it works the
 * same for block mode and streaming. It runs in-place with edge
 * extension — output length equals input length. */
ps_status_t ps2204a_set_resolution_enhancement(ps2204a_device_t *dev,
                                               int extra_bits);

/* ========================================================================
 * Equivalent-Time Sampling (ETS)
 * ========================================================================
 * Reconstruct a high-bandwidth view of a REPETITIVE signal by combining
 * many triggered block captures, each at a slightly different sub-sample
 * phase relative to the trigger. On the PS2204A the underlying ADC runs
 * at 100 MS/s (10 ns); ETS synthesises an effective 10/N ns grid.
 *
 * Implementation: software ETS. The driver captures N × cycles blocks at
 * tb=0 with an armed LEVEL trigger, estimates each block's fractional
 * trigger phase by linear interpolation of the two samples straddling
 * the threshold, bins samples into `interleaves` phase slots, and
 * averages across cycles. Requires a repetitive input and an armed
 * trigger on the source channel. */

typedef enum {
    PS_ETS_OFF  = 0,
    PS_ETS_FAST = 1,  /* Lower cycles, faster update (noisier) */
    PS_ETS_SLOW = 2   /* More cycles, lower noise */
} ps_ets_mode_t;

/* Enable ETS. `interleaves` (2..20) sets the effective-rate multiplier;
 * `cycles` (1..32) sets how many captures per bin are averaged. Either
 * can be 0 to accept mode-based defaults:
 *   PS_ETS_FAST → interleaves=10, cycles=2   (1 GS/s, ≈20 captures)
 *   PS_ETS_SLOW → interleaves=20, cycles=4   (2 GS/s, ≈80 captures)
 * Writes the effective per-sample interval (ps) to *out_interval_ps. */
ps_status_t ps2204a_set_ets(ps2204a_device_t *dev, ps_ets_mode_t mode,
                            int interleaves, int cycles,
                            int *out_interval_ps);

/* Capture an ETS reconstruction. n_samples is the per-block length
 * (1..8192); the output contains n_samples × interleaves samples at the
 * effective ETS interval. Writes that interval (ps) to *actual_interval_ps.
 * out_cap must be ≥ n_samples × interleaves. */
ps_status_t ps2204a_capture_ets(ps2204a_device_t *dev, int n_samples,
                                float *out_a, float *out_b,
                                int out_cap, int *actual_samples,
                                int *actual_interval_ps);

/* Disable ETS and resume normal block mode. */
ps_status_t ps2204a_disable_ets(ps2204a_device_t *dev);

/* ========================================================================
 * Block Capture
 * ======================================================================== */

/* Capture a single block of samples.
 * buf_a/buf_b: pre-allocated float arrays (size >= samples), NULL to skip.
 * actual_samples: number of samples actually captured.
 * Returns PS_OK on success. */
ps_status_t ps2204a_capture_block(ps2204a_device_t *dev, int samples,
                                  float *buf_a, float *buf_b,
                                  int *actual_samples);

/* ========================================================================
 * Streaming (fast block capture in dedicated thread)
 * ======================================================================== */

/* Start streaming. Launches a background thread.
 *   PS_STREAM_FAST:   rapid block captures (~330+ kS/s, small gaps between blocks)
 *   PS_STREAM_NATIVE: FPGA continuous mode (gap-free but hardware-limited to ~100 S/s)
 * ring_size: internal ring buffer size (0 = default 1M samples). */
ps_status_t ps2204a_start_streaming(ps2204a_device_t *dev,
                                    int sample_interval_us,
                                    ps_stream_cb_t callback,
                                    void *user_data,
                                    int ring_size);

/* Start streaming with explicit mode selection. */
ps_status_t ps2204a_start_streaming_mode(ps2204a_device_t *dev,
                                         ps_stream_mode_t mode,
                                         int sample_interval_us,
                                         ps_stream_cb_t callback,
                                         void *user_data,
                                         int ring_size);

/* Stop streaming and join the background thread. */
ps_status_t ps2204a_stop_streaming(ps2204a_device_t *dev);

/* Read the latest N samples from the ring buffer.
 * buf_a/buf_b: pre-allocated float arrays (size >= n), NULL to skip.
 * actual: number of samples actually returned. */
ps_status_t ps2204a_get_streaming_latest(ps2204a_device_t *dev,
                                         float *buf_a, float *buf_b,
                                         int n, int *actual);

/* Get streaming statistics. */
ps_status_t ps2204a_get_streaming_stats(ps2204a_device_t *dev,
                                        ps_stream_stats_t *stats);

/* Check if streaming is active. */
bool ps2204a_is_streaming(ps2204a_device_t *dev);

/* --- SDK continuous streaming (PS_STREAM_SDK) tuning ----------------------
 *
 * PS_STREAM_SDK replays the proprietary SDK's USB protocol to achieve
 * gap-free dual-channel capture. The two setters below tune the capture
 * parameters the SDK exposes.
 *
 * Call them AFTER open but BEFORE ps2204a_start_streaming_mode(PS_STREAM_SDK).
 * Mid-stream changes are ignored (cmd1 is baked into the streaming thread
 * at start time).
 */

/* Per-sample interval in nanoseconds.
 *   500       → 500 ns (2 MS/s)
 *   1000      → 1 µs   (1 MS/s, default)
 *   10000     → 10 µs  (100 kS/s)
 *   1000000   → 1 ms   (1 kS/s)
 *   0         → reset to default (1 µs)
 *
 * Accepted range: 500 ns .. 1 ms in steps of 10 ns (the device quantum).
 * Values outside that range return PS_ERROR_INVALID.
 *
 * Hardware-validated 500 ns → 1 ms, 2026-04-19. Slower rates work but each
 * chunk requires (8192 × interval_ns) wall time to flush, so consumers
 * should expect block latency = interval_ns × 8.192 ms.
 */
ps_status_t ps2204a_set_sdk_stream_interval_ns(ps2204a_device_t *dev,
                                               uint32_t interval_ns);

/* Automatic stop after N samples delivered.
 *   0             → disabled (free-running, the default)
 *   N (>0)        → stream stops once stream_samples_total ≥ N; the thread
 *                   exits cleanly and ps2204a_is_streaming() returns false.
 *
 * Actual stop point overshoots by ≤ the async transfer pool size
 * (≈ 32 KB / 4 = 8 k samples worst case, 0.8 % at 1 M samples).
 *
 * Client-side implementation (matches the SDK's behaviour — the device
 * itself has no auto_stop opcode).
 */
ps_status_t ps2204a_set_sdk_stream_auto_stop(ps2204a_device_t *dev,
                                             uint64_t max_samples);

/* ========================================================================
 * Signal Generator
 * ======================================================================== */

/* Set the built-in signal generator.
 *   frequency_hz: 0-100000 Hz
 *   pkpk_uv:      peak-to-peak amplitude in microvolts (e.g. 1000000 = 1 Vpp).
 *                 Pass 0 for the legacy default (1 Vpp).
 *
 * Implementation note: on this hardware the siggen is an AWG — wave type
 * and amplitude are encoded in an 8192-byte waveform LUT uploaded on EP 0x06
 * each call; only frequency lives in the control packet. */
ps_status_t ps2204a_set_siggen(ps2204a_device_t *dev, ps_wave_t type,
                               float frequency_hz, uint32_t pkpk_uv);

/* Disable signal generator. */
ps_status_t ps2204a_disable_siggen(ps2204a_device_t *dev);

/* Extended signal generator.
 *   start_hz / stop_hz:  if equal → fixed freq; else frequency sweep
 *   increment_hz:        step size for the sweep (0 = no sweep)
 *   dwell_s:             seconds to hold each step (up to 65535 / 187500 ≈ 0.35s)
 *   pkpk_uv:             peak-to-peak amplitude (µV), 0 → default 1 Vpp
 *   offset_uv:           DC offset (µV), signed. 0 = centred on 0V
 *   duty_pct:            SQUARE wave duty cycle 0..100 (default 50) */
ps_status_t ps2204a_set_siggen_ex(ps2204a_device_t *dev, ps_wave_t type,
                                  float start_hz, float stop_hz,
                                  float increment_hz, float dwell_s,
                                  uint32_t pkpk_uv, int32_t offset_uv,
                                  uint8_t duty_pct);

/* Upload an arbitrary waveform (int16 samples, up to 4096). Shorter inputs
 * are linearly resampled. The LUT is played back at the given frequency. */
ps_status_t ps2204a_set_siggen_arbitrary(ps2204a_device_t *dev,
                                         const int16_t *lut, int lut_n,
                                         float frequency_hz, uint32_t pkpk_uv);

/* Low-level: send siggen with an explicit raw freq_param (bypasses the
 * freq_hz → freq_param conversion). Used for protocol reverse-engineering. */
ps_status_t ps2204a_set_siggen_raw(ps2204a_device_t *dev, ps_wave_t type,
                                   uint32_t freq_param, uint32_t pkpk_uv);

/* ========================================================================
 * Info & Utilities
 * ======================================================================== */

/* Read device info (serial number, calibration date). */
ps_status_t ps2204a_get_info(ps2204a_device_t *dev, char *serial,
                             int serial_len, char *cal_date, int date_len);

/* ========================================================================
 * Calibration
 * ========================================================================
 * Every captured mV is post-processed as:
 *     out_mv = (raw_mv − offset_mv) × gain
 * with per-range (offset_mv, gain) tables. Defaults are (0.0, 1.0) — no
 * correction. Populate either by manual measurement or by calling
 * ps2204a_calibrate_dc_offset() with a grounded / shorted input. */
ps_status_t ps2204a_set_range_calibration(ps2204a_device_t *dev,
                                          ps_range_t range,
                                          float offset_mv, float gain);
ps_status_t ps2204a_get_range_calibration(ps2204a_device_t *dev,
                                          ps_range_t range,
                                          float *offset_mv, float *gain);

/* Auto-calibrate DC offset on the currently-enabled channels: assumes
 * their inputs are at 0 V and stores the measured mean as the offset. */
ps_status_t ps2204a_calibrate_dc_offset(ps2204a_device_t *dev);

/* Raw EEPROM bytes (256: pages 0x00, 0x40, 0x80, 0xC0). Used for anyone
 * reversing the PicoTech factory calibration layout. */
ps_status_t ps2204a_get_eeprom_raw(ps2204a_device_t *dev,
                                   uint8_t *out, int out_len);

/* Convert timebase index to sample interval in nanoseconds.
 * NOTE: This is the formula value (10 * 2^tb). In fast-streaming mode the
 * hardware ignores the requested timebase and samples at a fixed ~1280 ns;
 * use ps2204a_get_streaming_dt_ns() for the actual rate during streaming. */
int ps2204a_timebase_to_ns(int timebase);

/* Actual per-sample interval delivered by the hardware right now.
 * - Not streaming → falls back to ps2204a_timebase_to_ns(dev->timebase)
 * - PS_STREAM_FAST → 1280 ns (hardware samples at a fixed rate in this mode,
 *                              independent of the requested timebase)
 * - PS_STREAM_NATIVE → 10_000_000 ns (~100 S/s, hardware-capped)
 * - PS_STREAM_SDK    → 1_000 ns (1 MS/s, gap-free dual-channel) */
int ps2204a_get_streaming_dt_ns(const ps2204a_device_t *dev);

/* Max samples for current channel config (8064 single, 3968 dual). */
int ps2204a_max_samples(ps2204a_device_t *dev);

/* Diagnostic: capture a block and return the raw uint8 bytes from the
 * valid region of the 16 KB buffer (after header + circular-buffer
 * start-offset). Writes at most raw_cap bytes to raw_out; actual_bytes
 * receives the total valid length (which may exceed raw_cap).
 * In dual mode the data is tail-interleaved B,A,B,A,... */
ps_status_t ps2204a_capture_raw(ps2204a_device_t *dev, int samples,
                                uint8_t *raw_out, int raw_cap,
                                int *actual_bytes);

/* Testing hook: serialize the two 64-byte capture commands the driver
 * would send at the next capture, using the current channel / timebase /
 * trigger state. Lets the validation suite assert byte patterns against
 * reference SDK traces without needing a round-trip to hardware. */
ps_status_t ps2204a_debug_capture_cmds(ps2204a_device_t *dev, int samples,
                                       uint8_t cmd1_out[64],
                                       uint8_t cmd2_out[64]);

#ifdef __cplusplus
}
#endif

#endif /* PICOSCOPE2204A_H */
