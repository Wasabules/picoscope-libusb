// Shared C wrappers for the cgo layer.
//
// The `wrap_*` helpers are here rather than inline in one Go file's cgo
// preamble because cgo parses each Go file's preamble independently when
// type-checking `C.foo` references. A symbol defined in cgo.go's preamble
// is NOT automatically visible to app_*.go files that also import "C".
// Declaring them `static inline` in a header that every cgo file includes
// keeps the single-source-of-truth contract without linker collisions.

#ifndef PS_CGO_WRAPPERS_H
#define PS_CGO_WRAPPERS_H

#include "picoscope2204a.h"
#include <stdlib.h>
#include <stdbool.h>

static inline ps_status_t wrap_open(ps2204a_device_t **dev) {
    return ps2204a_open(dev);
}

static inline void wrap_close(ps2204a_device_t *dev) {
    ps2204a_close(dev);
}

static inline ps_status_t wrap_set_channel(ps2204a_device_t *dev, int ch,
                                            int enabled, int coupling, int range_val) {
    return ps2204a_set_channel(dev, (ps_channel_t)ch, (bool)enabled,
                               (ps_coupling_t)coupling, (ps_range_t)range_val);
}

static inline ps_status_t wrap_set_timebase(ps2204a_device_t *dev, int tb, int samples) {
    return ps2204a_set_timebase(dev, tb, samples);
}

static inline ps_status_t wrap_set_trigger(ps2204a_device_t *dev, int source,
                                            float threshold_mv, int dir,
                                            float delay_pct, int auto_trigger_ms) {
    return ps2204a_set_trigger(dev, (ps_channel_t)source, threshold_mv,
                               (ps_trigger_dir_t)dir, delay_pct, auto_trigger_ms);
}

static inline ps_status_t wrap_disable_trigger(ps2204a_device_t *dev) {
    return ps2204a_disable_trigger(dev);
}

static inline ps_status_t wrap_capture_block(ps2204a_device_t *dev, int samples,
                                              float *buf_a, float *buf_b, int *actual) {
    return ps2204a_capture_block(dev, samples, buf_a, buf_b, actual);
}

static inline ps_status_t wrap_start_streaming(ps2204a_device_t *dev, int interval_us,
                                                int ring_size) {
    return ps2204a_start_streaming(dev, interval_us, NULL, NULL, ring_size);
}

static inline ps_status_t wrap_start_streaming_mode(ps2204a_device_t *dev, int mode,
                                                     int interval_us, int ring_size) {
    return ps2204a_start_streaming_mode(dev, (ps_stream_mode_t)mode, interval_us,
                                        NULL, NULL, ring_size);
}

static inline ps_status_t wrap_capture_raw(ps2204a_device_t *dev, int samples,
                                            unsigned char *raw_out, int raw_cap,
                                            int *actual_bytes) {
    return ps2204a_capture_raw(dev, samples, raw_out, raw_cap, actual_bytes);
}

static inline ps_status_t wrap_stop_streaming(ps2204a_device_t *dev) {
    return ps2204a_stop_streaming(dev);
}

static inline int wrap_is_streaming(ps2204a_device_t *dev) {
    return ps2204a_is_streaming(dev) ? 1 : 0;
}

static inline ps_status_t wrap_get_streaming_latest(ps2204a_device_t *dev,
                                                     float *buf_a, float *buf_b,
                                                     int n, int *actual) {
    return ps2204a_get_streaming_latest(dev, buf_a, buf_b, n, actual);
}

static inline ps_status_t wrap_get_streaming_stats(ps2204a_device_t *dev,
                                                    ps_stream_stats_t *stats) {
    return ps2204a_get_streaming_stats(dev, stats);
}

static inline ps_status_t wrap_set_siggen(ps2204a_device_t *dev, int wave_type,
                                           float freq_hz, unsigned int pkpk_uv) {
    return ps2204a_set_siggen(dev, (ps_wave_t)wave_type, freq_hz,
                              (uint32_t)pkpk_uv);
}

static inline ps_status_t wrap_set_range_cal(ps2204a_device_t *dev, int range,
                                              float offset_mv, float gain) {
    return ps2204a_set_range_calibration(dev, (ps_range_t)range, offset_mv, gain);
}

static inline ps_status_t wrap_get_range_cal(ps2204a_device_t *dev, int range,
                                              float *offset_mv, float *gain) {
    return ps2204a_get_range_calibration(dev, (ps_range_t)range, offset_mv, gain);
}

static inline ps_status_t wrap_calibrate_dc_offset(ps2204a_device_t *dev) {
    return ps2204a_calibrate_dc_offset(dev);
}

static inline ps_status_t wrap_set_siggen_ex(ps2204a_device_t *dev, int wave_type,
                                              float start_hz, float stop_hz,
                                              float inc_hz, float dwell_s,
                                              unsigned int pkpk_uv, int offset_uv,
                                              int duty_pct) {
    return ps2204a_set_siggen_ex(dev, (ps_wave_t)wave_type,
                                  start_hz, stop_hz, inc_hz, dwell_s,
                                  (uint32_t)pkpk_uv, (int32_t)offset_uv,
                                  (uint8_t)duty_pct);
}

static inline ps_status_t wrap_disable_siggen(ps2204a_device_t *dev) {
    return ps2204a_disable_siggen(dev);
}

static inline ps_status_t wrap_get_info(ps2204a_device_t *dev, char *serial,
                                         int serial_len, char *cal_date, int date_len) {
    return ps2204a_get_info(dev, serial, serial_len, cal_date, date_len);
}

static inline int wrap_timebase_to_ns(int tb) {
    return ps2204a_timebase_to_ns(tb);
}

static inline int wrap_streaming_dt_ns(ps2204a_device_t *dev) {
    return ps2204a_get_streaming_dt_ns(dev);
}

static inline int wrap_max_samples(ps2204a_device_t *dev) {
    return ps2204a_max_samples(dev);
}

static inline ps_status_t wrap_set_sdk_stream_interval_ns(ps2204a_device_t *dev,
                                                           unsigned int interval_ns) {
    return ps2204a_set_sdk_stream_interval_ns(dev, (uint32_t)interval_ns);
}

static inline ps_status_t wrap_set_sdk_stream_auto_stop(ps2204a_device_t *dev,
                                                         unsigned long long max_samples) {
    return ps2204a_set_sdk_stream_auto_stop(dev, (uint64_t)max_samples);
}

/* ---- resolution enhancement ---- */
static inline ps_status_t wrap_set_resolution_enhancement(ps2204a_device_t *dev,
                                                          int extra_bits) {
    return ps2204a_set_resolution_enhancement(dev, extra_bits);
}

/* ---- overflow ---- */
static inline ps_status_t wrap_get_last_overflow(ps2204a_device_t *dev,
                                                 unsigned int *clipped_a,
                                                 unsigned int *clipped_b,
                                                 unsigned int *total) {
    ps_overflow_t ov = {0};
    ps_status_t st = ps2204a_get_last_overflow(dev, &ov);
    if (st != PS_OK) return st;
    if (clipped_a) *clipped_a = ov.clipped_a;
    if (clipped_b) *clipped_b = ov.clipped_b;
    if (total)     *total     = ov.total;
    return PS_OK;
}

/* ---- equivalent-time sampling ---- */
static inline ps_status_t wrap_set_ets(ps2204a_device_t *dev, int mode,
                                       int interleaves, int cycles,
                                       int *out_interval_ps) {
    uint32_t ips = 0;
    ps_status_t st = ps2204a_set_ets(dev, (ps_ets_mode_t)mode,
                                     interleaves, cycles, &ips);
    if (out_interval_ps) *out_interval_ps = (int)ips;
    return st;
}

static inline ps_status_t wrap_disable_ets(ps2204a_device_t *dev) {
    return ps2204a_disable_ets(dev);
}

static inline ps_status_t wrap_capture_ets(ps2204a_device_t *dev, int n_samples,
                                           float *buf_a, float *buf_b,
                                           int out_cap, int *actual,
                                           int *interval_ps) {
    return ps2204a_capture_ets(dev, n_samples, buf_a, buf_b, out_cap,
                               actual, interval_ps);
}

/* ---- advanced triggers ---- */
static inline ps_status_t wrap_set_trigger_ex(ps2204a_device_t *dev, int source,
                                              float threshold_mv, int dir,
                                              float delay_pct, int auto_ms,
                                              int hysteresis) {
    return ps2204a_set_trigger_ex(dev, (ps_channel_t)source, threshold_mv,
                                  (ps_trigger_dir_t)dir, delay_pct, auto_ms,
                                  hysteresis);
}

static inline ps_status_t wrap_set_trigger_window(ps2204a_device_t *dev, int source,
                                                  float lower_mv, float upper_mv,
                                                  int dir, float delay_pct,
                                                  int auto_ms) {
    return ps2204a_set_trigger_window(dev, (ps_channel_t)source, lower_mv,
                                      upper_mv, (ps_trigger_dir_t)dir,
                                      delay_pct, auto_ms);
}

/* ---- arbitrary waveform ---- */
static inline ps_status_t wrap_set_siggen_arbitrary(ps2204a_device_t *dev,
                                                    const short *lut, int lut_n,
                                                    float frequency_hz,
                                                    unsigned int pkpk_uv) {
    return ps2204a_set_siggen_arbitrary(dev, (const int16_t *)lut, lut_n,
                                        frequency_hz, (uint32_t)pkpk_uv);
}

static inline ps_status_t wrap_set_trigger_pwq(ps2204a_device_t *dev, int source,
                                               float threshold_mv, int dir,
                                               int lower_ns, int upper_ns,
                                               float delay_pct, int auto_ms) {
    return ps2204a_set_trigger_pwq(dev, (ps_channel_t)source, threshold_mv,
                                   (ps_trigger_dir_t)dir, lower_ns, upper_ns,
                                   delay_pct, auto_ms);
}

/* ---- aggregated streaming ---- */
static inline ps_status_t wrap_get_streaming_aggregated(ps2204a_device_t *dev,
                                                        float *min_a, float *max_a,
                                                        float *min_b, float *max_b,
                                                        int n_buckets, int span,
                                                        int *actual) {
    return ps2204a_get_streaming_aggregated(dev, min_a, max_a, min_b, max_b,
                                            n_buckets, span, actual);
}

/* ---- EEPROM calibration ---- */
static inline ps_status_t wrap_get_eeprom_cal(ps2204a_device_t *dev,
                                              int *valid, float *offset_mv,
                                              float *gain_a, float *gain_b) {
    ps_cal_table_t cal;
    ps_status_t st = ps2204a_get_eeprom_calibration(dev, &cal);
    if (st != PS_OK) return st;
    if (valid) *valid = cal.valid ? 1 : 0;
    for (int r = 0; r < PS2204A_NUM_RANGES; r++) {
        if (offset_mv) offset_mv[r] = cal.offset_mv[r];
        if (gain_a)    gain_a[r]    = cal.gain[0][r];
        if (gain_b)    gain_b[r]    = cal.gain[1][r];
    }
    return PS_OK;
}

static inline ps_status_t wrap_use_eeprom_cal(ps2204a_device_t *dev, int enable) {
    return ps2204a_use_eeprom_calibration(dev, enable != 0);
}

static inline int wrap_eeprom_cal_active(ps2204a_device_t *dev) {
    return ps2204a_eeprom_calibration_active(dev) ? 1 : 0;
}

#endif // PS_CGO_WRAPPERS_H
