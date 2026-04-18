package main

/*
#cgo CFLAGS: -I${SRCDIR}/../driver
#cgo LDFLAGS: ${SRCDIR}/../driver/libpicoscope2204a.a -lusb-1.0 -lpthread -lm
#include "picoscope2204a.h"
#include <stdlib.h>
#include <stdbool.h>

// C helper wrappers to avoid CGo type conversion issues

static ps_status_t wrap_open(ps2204a_device_t **dev) {
    return ps2204a_open(dev);
}

static void wrap_close(ps2204a_device_t *dev) {
    ps2204a_close(dev);
}

static ps_status_t wrap_set_channel(ps2204a_device_t *dev, int ch,
                                     int enabled, int coupling, int range_val) {
    return ps2204a_set_channel(dev, (ps_channel_t)ch, (bool)enabled,
                               (ps_coupling_t)coupling, (ps_range_t)range_val);
}

static ps_status_t wrap_set_timebase(ps2204a_device_t *dev, int tb, int samples) {
    return ps2204a_set_timebase(dev, tb, samples);
}

static ps_status_t wrap_set_trigger(ps2204a_device_t *dev, int source,
                                     float threshold_mv, int dir,
                                     float delay_pct, int auto_trigger_ms) {
    return ps2204a_set_trigger(dev, (ps_channel_t)source, threshold_mv,
                               (ps_trigger_dir_t)dir, delay_pct, auto_trigger_ms);
}

static ps_status_t wrap_disable_trigger(ps2204a_device_t *dev) {
    return ps2204a_disable_trigger(dev);
}

static ps_status_t wrap_capture_block(ps2204a_device_t *dev, int samples,
                                       float *buf_a, float *buf_b, int *actual) {
    return ps2204a_capture_block(dev, samples, buf_a, buf_b, actual);
}

static ps_status_t wrap_start_streaming(ps2204a_device_t *dev, int interval_us,
                                         int ring_size) {
    return ps2204a_start_streaming(dev, interval_us, NULL, NULL, ring_size);
}

static ps_status_t wrap_start_streaming_mode(ps2204a_device_t *dev, int mode,
                                              int interval_us, int ring_size) {
    return ps2204a_start_streaming_mode(dev, (ps_stream_mode_t)mode, interval_us,
                                        NULL, NULL, ring_size);
}

static ps_status_t wrap_capture_raw(ps2204a_device_t *dev, int samples,
                                     unsigned char *raw_out, int raw_cap,
                                     int *actual_bytes) {
    return ps2204a_capture_raw(dev, samples, raw_out, raw_cap, actual_bytes);
}

static ps_status_t wrap_stop_streaming(ps2204a_device_t *dev) {
    return ps2204a_stop_streaming(dev);
}

static int wrap_is_streaming(ps2204a_device_t *dev) {
    return ps2204a_is_streaming(dev) ? 1 : 0;
}

static ps_status_t wrap_get_streaming_latest(ps2204a_device_t *dev,
                                              float *buf_a, float *buf_b,
                                              int n, int *actual) {
    return ps2204a_get_streaming_latest(dev, buf_a, buf_b, n, actual);
}

static ps_status_t wrap_get_streaming_stats(ps2204a_device_t *dev,
                                             ps_stream_stats_t *stats) {
    return ps2204a_get_streaming_stats(dev, stats);
}

static ps_status_t wrap_set_siggen(ps2204a_device_t *dev, int wave_type,
                                    float freq_hz, unsigned int pkpk_uv) {
    return ps2204a_set_siggen(dev, (ps_wave_t)wave_type, freq_hz,
                              (uint32_t)pkpk_uv);
}

static ps_status_t wrap_set_range_cal(ps2204a_device_t *dev, int range,
                                       float offset_mv, float gain) {
    return ps2204a_set_range_calibration(dev, (ps_range_t)range, offset_mv, gain);
}

static ps_status_t wrap_get_range_cal(ps2204a_device_t *dev, int range,
                                       float *offset_mv, float *gain) {
    return ps2204a_get_range_calibration(dev, (ps_range_t)range, offset_mv, gain);
}

static ps_status_t wrap_calibrate_dc_offset(ps2204a_device_t *dev) {
    return ps2204a_calibrate_dc_offset(dev);
}

static ps_status_t wrap_set_siggen_ex(ps2204a_device_t *dev, int wave_type,
                                       float start_hz, float stop_hz,
                                       float inc_hz, float dwell_s,
                                       unsigned int pkpk_uv, int offset_uv,
                                       int duty_pct) {
    return ps2204a_set_siggen_ex(dev, (ps_wave_t)wave_type,
                                  start_hz, stop_hz, inc_hz, dwell_s,
                                  (uint32_t)pkpk_uv, (int32_t)offset_uv,
                                  (uint8_t)duty_pct);
}

static ps_status_t wrap_disable_siggen(ps2204a_device_t *dev) {
    return ps2204a_disable_siggen(dev);
}

static ps_status_t wrap_get_info(ps2204a_device_t *dev, char *serial,
                                  int serial_len, char *cal_date, int date_len) {
    return ps2204a_get_info(dev, serial, serial_len, cal_date, date_len);
}

static int wrap_timebase_to_ns(int tb) {
    return ps2204a_timebase_to_ns(tb);
}

static int wrap_streaming_dt_ns(ps2204a_device_t *dev) {
    return ps2204a_get_streaming_dt_ns(dev);
}

static int wrap_max_samples(ps2204a_device_t *dev) {
    return ps2204a_max_samples(dev);
}
*/
import "C"

import (
	"context"
	"encoding/base64"
	"fmt"
	"os"
	"strings"
	"sync"
	"time"
	"unsafe"

	"gui/decoder"

	wailsRuntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

const (
	maxSamplesSingle = 8064
	maxSamplesDual   = 3968
	// Driver uses the TB_CHAN lookup for tb 0..10 (SDK-verified) and a
	// progressive divide-by-2 extrapolation for 11..23. At very high tb each
	// block takes seconds, so streaming polling is sized accordingly.
	timebaseMax = 23
)

/* ======================================================================== */
/* Types exported to frontend via JSON                                      */
/* ======================================================================== */

type DeviceInfo struct {
	Connected bool   `json:"connected"`
	Serial    string `json:"serial"`
	CalDate   string `json:"calDate"`
}

type ChannelConfig struct {
	Enabled  bool   `json:"enabled"`
	Coupling string `json:"coupling"`
	Range    string `json:"range"`
}

type WaveformData struct {
	ChannelA   []float64 `json:"channelA"`
	ChannelB   []float64 `json:"channelB"`
	NumSamples int       `json:"numSamples"`
	Timebase   int       `json:"timebase"`
	TimebaseNs int       `json:"timebaseNs"`
	RangeMvA   int       `json:"rangeMvA"`
	RangeMvB   int       `json:"rangeMvB"`
}

type StreamStats struct {
	Blocks        uint64  `json:"blocks"`
	TotalSamples  uint64  `json:"totalSamples"`
	ElapsedS      float64 `json:"elapsedS"`
	SamplesPerSec float64 `json:"samplesPerSec"`
	BlocksPerSec  float64 `json:"blocksPerSec"`
	LastBlockMs   float64 `json:"lastBlockMs"`
	Mode          string  `json:"mode"`
}

type TimebaseInfo struct {
	Index      int    `json:"index"`
	IntervalNs int    `json:"intervalNs"`
	Label      string `json:"label"`
}

type Measurements struct {
	MinMv  float64 `json:"minMv"`
	MaxMv  float64 `json:"maxMv"`
	MeanMv float64 `json:"meanMv"`
	VppMv  float64 `json:"vppMv"`
}

/* ======================================================================== */
/* Range helpers                                                            */
/* ======================================================================== */

var rangeEnumVal = map[string]int{
	"50mV": 2, "100mV": 3, "200mV": 4, "500mV": 5,
	"1V": 6, "2V": 7, "5V": 8, "10V": 9, "20V": 10,
}

// Effective ADC full-scale range in mV (must match C driver RANGE_MV[]).
// 50mV uses digital scaling (same PGA as 5V, host windows to ±50 mV).
// 5V and 10V are calibrated empirically (bank 0 PGA gains).
var rangeMvVal = map[string]int{
	"50mV": 50, "100mV": 100, "200mV": 200, "500mV": 500,
	"1V": 1000, "2V": 2000, "5V": 3515, "10V": 9092, "20V": 20000,
}

// Streaming mode constants (must match ps_stream_mode_t in picoscope2204a.h)
const (
	streamModeFast   = 0
	streamModeNative = 1
	streamModeSDK    = 2
)

// Default ring buffer sizes per mode.
const (
	ringFast   = 1 << 20 // 1 M samples (~3 s at 330 kS/s)
	ringNative = 1 << 14 // 16 k samples (~160 s at 100 S/s)
	ringSDK    = 1 << 22 // 4 M samples (~4 s at 1 MS/s)
)

/* ======================================================================== */
/* App struct                                                               */
/* ======================================================================== */

type App struct {
	ctx context.Context
	dev *C.ps2204a_device_t
	mu  sync.Mutex

	connected bool
	serial    string
	calDate   string

	chA ChannelConfig
	chB ChannelConfig

	timebase int
	samples  int

	streaming  bool
	streamMode int // streamModeFast or streamModeNative
	stopChan   chan struct{}

	// Pre-allocated C buffers for streaming poll, sized for several blocks
	// so a slow Go poll doesn't drop samples when the ring has grown past
	// one block since last read.
	streamBufA     *C.float
	streamBufB     *C.float
	streamBufCap   int    // capacity of streamBufA/B in floats
	streamLastSeen uint64 // ring total_samples at last successful poll
	sdkForcedChB   bool   // SDK mode forced CH B on; restore on stop

	// Streaming protocol decoder (optional). Mutated from the streaming
	// goroutine; gated by `a.mu` like the rest of `App`.
	decSession *decoder.Session
}

func NewApp() *App {
	return &App{
		chA:      ChannelConfig{Enabled: true, Coupling: "DC", Range: "5V"},
		chB:      ChannelConfig{Enabled: false, Coupling: "DC", Range: "5V"},
		timebase: 5,
		samples:  maxSamplesSingle,
	}
}

// effectiveMaxSamples returns the maximum sample count for the current
// channel configuration.
func (a *App) effectiveMaxSamples() int {
	if a.chA.Enabled && a.chB.Enabled {
		return maxSamplesDual
	}
	return maxSamplesSingle
}

func (a *App) startup(ctx context.Context) {
	a.ctx = ctx
}

func (a *App) shutdown(ctx context.Context) {
	if a.streaming {
		a.StopStreaming()
	}
	if a.connected {
		a.Disconnect()
	}
}

/* ======================================================================== */
/* Connection                                                               */
/* ======================================================================== */

func (a *App) Connect() (DeviceInfo, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.connected {
		return DeviceInfo{Connected: true, Serial: a.serial, CalDate: a.calDate}, nil
	}

	st := C.wrap_open(&a.dev)
	if st != 0 {
		return DeviceInfo{}, fmt.Errorf("failed to open device (status=%d)", int(st))
	}

	var serial [32]C.char
	var calDate [32]C.char
	C.wrap_get_info(a.dev, &serial[0], 32, &calDate[0], 32)
	a.serial = C.GoString(&serial[0])
	a.calDate = C.GoString(&calDate[0])
	a.connected = true

	// Apply default channel config
	a.applyChannelLocked(0, a.chA)
	a.applyChannelLocked(1, a.chB)
	C.wrap_set_timebase(a.dev, C.int(a.timebase), C.int(a.samples))

	return DeviceInfo{Connected: true, Serial: a.serial, CalDate: a.calDate}, nil
}

func (a *App) Disconnect() {
	a.mu.Lock()
	defer a.mu.Unlock()

	if a.streaming {
		a.stopStreamingLocked()
	}
	if a.dev != nil {
		C.wrap_close(a.dev)
		a.dev = nil
	}
	a.connected = false
}

func (a *App) IsConnected() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.connected
}

func (a *App) GetDeviceInfo() DeviceInfo {
	a.mu.Lock()
	defer a.mu.Unlock()
	return DeviceInfo{Connected: a.connected, Serial: a.serial, CalDate: a.calDate}
}

/* ======================================================================== */
/* Channel configuration                                                    */
/* ======================================================================== */

func (a *App) applyChannelLocked(ch int, cfg ChannelConfig) {
	if a.dev == nil {
		return
	}
	en := 0
	if cfg.Enabled {
		en = 1
	}
	coupling := 1 // DC
	if cfg.Coupling == "AC" {
		coupling = 0
	}
	rv, ok := rangeEnumVal[cfg.Range]
	if !ok {
		rv = 8 // PS_5V
	}
	C.wrap_set_channel(a.dev, C.int(ch), C.int(en), C.int(coupling), C.int(rv))
}

func (a *App) SetChannelA(enabled bool, coupling string, rangeStr string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.chA = ChannelConfig{Enabled: enabled, Coupling: coupling, Range: rangeStr}
	if a.connected {
		a.applyChannelLocked(0, a.chA)
	}
	a.clampSamplesLocked()
	return nil
}

func (a *App) SetChannelB(enabled bool, coupling string, rangeStr string) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	a.chB = ChannelConfig{Enabled: enabled, Coupling: coupling, Range: rangeStr}
	if a.connected {
		a.applyChannelLocked(1, a.chB)
	}
	a.clampSamplesLocked()
	return nil
}

// clampSamplesLocked ensures the current sample count is valid for the
// current channel config; reapplies timebase if it changes.
func (a *App) clampSamplesLocked() {
	m := a.effectiveMaxSamples()
	if a.samples > m {
		a.samples = m
		if a.connected {
			C.wrap_set_timebase(a.dev, C.int(a.timebase), C.int(a.samples))
		}
	}
}

func (a *App) GetChannelA() ChannelConfig { return a.chA }
func (a *App) GetChannelB() ChannelConfig { return a.chB }

/* ======================================================================== */
/* Timebase                                                                 */
/* ======================================================================== */

func (a *App) SetTimebase(timebase int) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if timebase < 0 || timebase > timebaseMax {
		return fmt.Errorf("invalid timebase %d (0-%d)", timebase, timebaseMax)
	}
	a.timebase = timebase
	if a.connected {
		C.wrap_set_timebase(a.dev, C.int(timebase), C.int(a.samples))
	}

	// If streaming is active, restart it so the new timebase actually
	// takes effect — the C thread pre-builds cmd1/cmd2 from dev->timebase
	// at startup and never refreshes them, so a mid-stream change is
	// otherwise ignored.
	if a.streaming {
		mode := streamModeName(a.streamMode)
		a.stopStreamingLocked()
		a.mu.Unlock()
		err := a.StartStreamingMode(mode)
		a.mu.Lock()
		if err != nil {
			return fmt.Errorf("restart streaming after timebase change: %w", err)
		}
	}
	return nil
}

// SetSamples sets the number of samples captured per block. Clamped to
// the driver's valid range (1..8064 single-channel or 1..3968 dual).
func (a *App) SetSamples(samples int) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	m := a.effectiveMaxSamples()
	if samples < 64 {
		samples = 64
	}
	if samples > m {
		samples = m
	}
	a.samples = samples
	if a.connected {
		C.wrap_set_timebase(a.dev, C.int(a.timebase), C.int(a.samples))
	}
	return nil
}

func (a *App) GetSamples() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.samples
}

func (a *App) GetMaxSamples() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.effectiveMaxSamples()
}

func (a *App) GetTimebase() int { return a.timebase }

func (a *App) GetTimebaseNs(tb int) int {
	return int(C.wrap_timebase_to_ns(C.int(tb)))
}

func (a *App) GetAllTimebases() []TimebaseInfo {
	result := make([]TimebaseInfo, timebaseMax+1)
	for i := 0; i <= timebaseMax; i++ {
		ns := int(C.wrap_timebase_to_ns(C.int(i)))
		var label string
		if ns < 1000 {
			label = fmt.Sprintf("%d ns", ns)
		} else if ns < 1000000 {
			label = fmt.Sprintf("%.1f us", float64(ns)/1000)
		} else {
			label = fmt.Sprintf("%.1f ms", float64(ns)/1e6)
		}
		result[i] = TimebaseInfo{Index: i, IntervalNs: ns, Label: label}
	}
	return result
}

/* ======================================================================== */
/* Trigger                                                                  */
/* ======================================================================== */

func (a *App) SetTrigger(source string, thresholdMv float64, direction string,
	delayPct float64, autoMs int) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return fmt.Errorf("not connected")
	}

	ch := 0
	if source == "B" {
		ch = 1
	}
	dir := 0
	if direction == "falling" {
		dir = 1
	}

	st := C.wrap_set_trigger(a.dev, C.int(ch), C.float(thresholdMv),
		C.int(dir), C.float(delayPct), C.int(autoMs))
	if st != 0 {
		return fmt.Errorf("set_trigger failed (status=%d)", int(st))
	}
	return nil
}

func (a *App) DisableTrigger() error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return fmt.Errorf("not connected")
	}
	C.wrap_disable_trigger(a.dev)
	return nil
}

/* ======================================================================== */
/* Block Capture                                                            */
/* ======================================================================== */

func (a *App) CaptureBlock() (*WaveformData, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return nil, fmt.Errorf("not connected")
	}

	m := a.effectiveMaxSamples()
	n := a.samples
	if n > m {
		n = m
	}
	bufA := (*C.float)(C.malloc(C.size_t(n) * 4))
	defer C.free(unsafe.Pointer(bufA))

	var bufB *C.float
	if a.chB.Enabled {
		bufB = (*C.float)(C.malloc(C.size_t(n) * 4))
		defer C.free(unsafe.Pointer(bufB))
	}

	var actual C.int
	st := C.wrap_capture_block(a.dev, C.int(n), bufA, bufB, &actual)
	if st != 0 {
		return nil, fmt.Errorf("capture failed (status=%d)", int(st))
	}

	na := int(actual)
	data := &WaveformData{
		NumSamples: na,
		Timebase:   a.timebase,
		TimebaseNs: int(C.wrap_timebase_to_ns(C.int(a.timebase))),
		RangeMvA:   rangeMvVal[a.chA.Range],
		RangeMvB:   rangeMvVal[a.chB.Range],
	}

	if a.chA.Enabled {
		data.ChannelA = cFloatsToGoSlice(bufA, na)
	}
	if a.chB.Enabled && bufB != nil {
		data.ChannelB = cFloatsToGoSlice(bufB, na)
	}

	return data, nil
}

func cFloatsToGoSlice(cBuf *C.float, n int) []float64 {
	if n <= 0 || cBuf == nil {
		return nil
	}
	result := make([]float64, n)
	cSlice := (*[1 << 28]C.float)(unsafe.Pointer(cBuf))[:n:n]
	for i := 0; i < n; i++ {
		result[i] = float64(cSlice[i])
	}
	return result
}

// RawCaptureData holds the raw uint8 bytes returned by the device after a
// single capture. Useful for diagnosing dual-channel layout or PGA
// behaviour.
type RawCaptureData struct {
	Bytes      []int `json:"bytes"`
	NumBytes   int   `json:"numBytes"`
	NumSamples int   `json:"numSamples"`
	Timebase   int   `json:"timebase"`
	TimebaseNs int   `json:"timebaseNs"`
	Dual       bool  `json:"dual"`
}

// CaptureRaw performs a single block capture and returns the raw 8-bit
// samples from the valid segment of the 16 KB USB buffer. In dual-channel
// mode the data is tail-interleaved B,A,B,A,... (even offset = CH B).
func (a *App) CaptureRaw() (*RawCaptureData, error) {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return nil, fmt.Errorf("not connected")
	}

	n := a.samples
	if n > a.effectiveMaxSamples() {
		n = a.effectiveMaxSamples()
	}

	// Allocate worst-case 16 KB (entire USB buffer minus header).
	const rawCap = 16384
	raw := (*C.uchar)(C.malloc(rawCap))
	defer C.free(unsafe.Pointer(raw))

	var actual C.int
	st := C.wrap_capture_raw(a.dev, C.int(n), raw, C.int(rawCap), &actual)
	if st != 0 {
		return nil, fmt.Errorf("capture_raw failed (status=%d)", int(st))
	}

	na := int(actual)
	if na > rawCap {
		na = rawCap
	}
	bytes := make([]int, na)
	cSlice := (*[rawCap]C.uchar)(unsafe.Pointer(raw))[:na:na]
	for i := 0; i < na; i++ {
		bytes[i] = int(cSlice[i])
	}

	return &RawCaptureData{
		Bytes:      bytes,
		NumBytes:   na,
		NumSamples: n,
		Timebase:   a.timebase,
		TimebaseNs: int(C.wrap_timebase_to_ns(C.int(a.timebase))),
		Dual:       a.chA.Enabled && a.chB.Enabled,
	}, nil
}

/* ======================================================================== */
/* Streaming                                                                */
/* ======================================================================== */

// StartStreaming starts streaming in the default (fast block) mode.
// Kept for backward compatibility with existing frontend code.
func (a *App) StartStreaming() error {
	return a.StartStreamingMode("fast")
}

// StartStreamingMode starts streaming with an explicit mode:
//   - "fast":   rapid block captures (~330 kS/s), supports dual channel
//   - "native": FPGA continuous mode (~100 S/s, CH A only, gap-free)
//   - "sdk":    SDK continuous mode (1 MS/s dual channel, gap-free) — use for UART
func (a *App) StartStreamingMode(mode string) error {
	a.mu.Lock()
	if !a.connected {
		a.mu.Unlock()
		return fmt.Errorf("not connected")
	}
	if a.streaming {
		a.mu.Unlock()
		return nil
	}

	modeInt := streamModeFast
	ringSize := ringFast
	switch mode {
	case "native":
		modeInt = streamModeNative
		ringSize = ringNative
	case "sdk":
		modeInt = streamModeSDK
		ringSize = ringSDK
	case "fast", "":
		modeInt = streamModeFast
		ringSize = ringFast
	default:
		a.mu.Unlock()
		return fmt.Errorf("unknown streaming mode %q", mode)
	}

	// PS_STREAM_SDK halves its hardware sample rate (500 kS/s instead of
	// 1 MS/s) when CH B is disabled in the gain bytes — the SDK trace was
	// captured with both channels on and the FPGA configures its ADC
	// pipeline accordingly. Force CH B on at the driver level for the
	// duration of the stream; chB.Enabled is preserved so stopStreaming can
	// restore the user-facing state.
	sdkForcedChB := false
	if modeInt == streamModeSDK && !a.chB.Enabled {
		rv, ok := rangeEnumVal[a.chA.Range]
		if !ok {
			rv = 8
		}
		C.wrap_set_channel(a.dev, 1, 1, 1, C.int(rv))
		sdkForcedChB = true
	}
	a.sdkForcedChB = sdkForcedChB

	st := C.wrap_start_streaming_mode(a.dev, C.int(modeInt), 1, C.int(ringSize))
	if st != 0 {
		if sdkForcedChB {
			a.applyChannelLocked(1, a.chB)
			a.sdkForcedChB = false
		}
		a.mu.Unlock()
		return fmt.Errorf("start_streaming failed (status=%d)", int(st))
	}

	// Pre-allocate C buffers. Native mode only delivers CH A.
	// Size: must match (or exceed) the C ring capacity so a stalled poll
	// can drain everything the hardware produced without truncating —
	// get_streaming_latest() keeps only the last N samples, so any cap
	// below the accumulated-since-last-drain count creates a hole at the
	// oldest end of the timeline. Fast mode produces ~781 kS/s; a 200 ms
	// GC pause or Wails IPC stall would accumulate ~156 k samples, which
	// blows the old 128 k cap and silently drops the earliest ~30 k.
	// Sizing at ringFast (1 M samples) eliminates that hole up to a full
	// 1.28 s stall. Memory impact: ~8 MB per channel.
	bufCap := ringFast
	if modeInt == streamModeSDK {
		bufCap = ringSDK
	}
	a.streamBufCap = bufCap
	a.streamLastSeen = 0
	a.streamBufA = (*C.float)(C.malloc(C.size_t(bufCap) * 4))
	if (modeInt == streamModeFast || modeInt == streamModeSDK) && a.chB.Enabled {
		a.streamBufB = (*C.float)(C.malloc(C.size_t(bufCap) * 4))
	}

	a.streaming = true
	a.streamMode = modeInt
	a.stopChan = make(chan struct{})
	a.mu.Unlock()

	go a.streamingPollLoop()
	return nil
}

func (a *App) streamingPollLoop() {
	// Poll interval depends on mode and timebase.
	//   Native  : hardware-limited to ~100 S/s, 200 ms is plenty.
	//   Fast tb≤10 : ~330 kS/s, 33 ms keeps the UI fluid.
	//   Fast tb≥11 : each block takes >80 ms (tb=11) up to minutes (tb=20+),
	//                so poll every 250 ms — anything faster just wastes CPU.
	a.mu.Lock()
	interval := 33 * time.Millisecond
	if a.streamMode == streamModeNative {
		interval = 200 * time.Millisecond
	} else if a.streamMode == streamModeSDK {
		// 1 MS/s ⇒ ~33k samples per 33ms poll. Keep UI fluid without burning
		// CPU; a faster poll gives no benefit because data streams gap-free.
		interval = 33 * time.Millisecond
	} else if a.timebase >= 11 {
		interval = 250 * time.Millisecond
	}
	bufCap := a.streamBufCap
	a.mu.Unlock()

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			a.mu.Lock()
			if !a.streaming || a.dev == nil {
				a.mu.Unlock()
				return
			}

			// Detect C-side thread death (fatal USB error, allocation
			// failure, etc.). In that case the driver cleared its
			// streaming flag internally; we need to tear down the Go
			// side and notify the frontend so the UI can re-enable
			// controls instead of appearing to stream forever.
			if C.wrap_is_streaming(a.dev) == 0 {
				a.stopStreamingLocked()
				a.mu.Unlock()
				wailsRuntime.EventsEmit(a.ctx, "streamStopped",
					map[string]string{"reason": "driver thread exited"})
				return
			}

			// Query stats first to learn how many samples the C ring has
			// accumulated total. `get_streaming_latest` returns the LAST N
			// entries in the ring — if we ask for fewer than the number of
			// samples written since last poll, we silently drop the oldest
			// ones. Use (total_now - last_seen) to pull exactly the new
			// region, capped at our buffer size.
			var cStats C.ps_stream_stats_t
			C.wrap_get_streaming_stats(a.dev, &cStats)

			totalNow := uint64(cStats.total_samples)
			newCount := uint64(0)
			if totalNow > a.streamLastSeen {
				newCount = totalNow - a.streamLastSeen
			}
			readN := int(newCount)
			if readN > bufCap {
				// Poll took too long and the ring has grown past our
				// buffer — we'll only get the most recent `bufCap` and
				// lose a bit of history. Rare unless the UI thread stalls.
				readN = bufCap
			}
			var actual C.int
			if readN > 0 {
				C.wrap_get_streaming_latest(a.dev, a.streamBufA, a.streamBufB,
					C.int(readN), &actual)
			}
			na := int(actual)
			a.streamLastSeen = totalNow

			var dataA, dataB []float64
			if na > 0 && a.chA.Enabled {
				dataA = cFloatsToGoSlice(a.streamBufA, na)
			}
			if na > 0 && (a.streamMode == streamModeFast || a.streamMode == streamModeSDK) &&
				a.chB.Enabled && a.streamBufB != nil {
				dataB = cFloatsToGoSlice(a.streamBufB, na)
			}

			tb := a.timebase
			chARng := a.chA.Range
			chBRng := a.chB.Range
			mode := a.streamMode
			a.mu.Unlock()

			if na > 0 {
				// Fast-streaming samples at a fixed ~1280 ns regardless of
				// the requested timebase (the cmd1 tb bytes are ignored by
				// the FPGA in that mode). Use the driver's observed rate —
				// ps2204a_timebase_to_ns(tb) would be off by 4× at tb=5 and
				// make a 1 kHz input look like 4 kHz on the trace.
				dtNs := int(C.wrap_streaming_dt_ns(a.dev))
				if dtNs <= 0 {
					dtNs = int(C.wrap_timebase_to_ns(C.int(tb)))
				}
				wailsRuntime.EventsEmit(a.ctx, "waveform", WaveformData{
					ChannelA:   dataA,
					ChannelB:   dataB,
					NumSamples: na,
					Timebase:   tb,
					TimebaseNs: dtNs,
					RangeMvA:   rangeMvVal[chARng],
					RangeMvB:   rangeMvVal[chBRng],
				})

				// Streaming decoder. Between two hardware capture blocks
				// the scope loses ~5.7 ms of signal, so samples either
				// side of the boundary are NOT contiguous on the wire
				// even though they arrive in one contiguous Go slice.
				// Slice the drained buffer at block_size boundaries and
				// feed the session one hardware block at a time, calling
				// ResetTail between blocks so the decoder never stitches
				// a UART frame across the real-signal gap (which was
				// producing phantom bytes at block seams).
				a.mu.Lock()
				sess := a.decSession
				a.mu.Unlock()
				if sess != nil {
					blockSize := maxSamplesSingle
					if a.chA.Enabled && a.chB.Enabled {
						blockSize = maxSamplesDual
					}
					if mode == streamModeNative || mode == streamModeSDK || blockSize <= 0 {
						// Native and SDK modes are truly gap-free — no
						// per-block seams, so feed as a continuous stream
						// and skip the ResetTail dance the fast-block path
						// needs between 8064-sample chunks.
						ev := sess.Feed(dataA, dataB, float64(dtNs),
							float64(rangeMvVal[chARng]),
							float64(rangeMvVal[chBRng]))
						if len(ev) > 0 {
							wailsRuntime.EventsEmit(a.ctx, "decoderEvents", ev)
						}
					} else {
						for off := 0; off < len(dataA); off += blockSize {
							end := off + blockSize
							if end > len(dataA) {
								end = len(dataA)
							}
							var chunkA []float64
							chunkA = dataA[off:end]
							var chunkB []float64
							if len(dataB) >= end {
								chunkB = dataB[off:end]
							}
							ev := sess.Feed(chunkA, chunkB, float64(dtNs),
								float64(rangeMvVal[chARng]),
								float64(rangeMvVal[chBRng]))
							if len(ev) > 0 {
								wailsRuntime.EventsEmit(a.ctx, "decoderEvents", ev)
							}
							// Drop cross-block history — the next chunk
							// is from a fresh capture and its first
							// samples are not adjacent in wire-time.
							sess.ResetTail()
						}
					}
				}
			}

			wailsRuntime.EventsEmit(a.ctx, "streamStats", StreamStats{
				Blocks:        uint64(cStats.blocks),
				TotalSamples:  uint64(cStats.total_samples),
				ElapsedS:      float64(cStats.elapsed_s),
				SamplesPerSec: float64(cStats.samples_per_sec),
				BlocksPerSec:  float64(cStats.blocks_per_sec),
				LastBlockMs:   float64(cStats.last_block_ms),
				Mode:          streamModeName(mode),
			})

		case <-a.stopChan:
			return
		}
	}
}

func streamModeName(m int) string {
	switch m {
	case streamModeNative:
		return "native"
	case streamModeSDK:
		return "sdk"
	default:
		return "fast"
	}
}

func (a *App) StopStreaming() error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.streaming {
		return nil
	}
	a.stopStreamingLocked()
	return nil
}

func (a *App) stopStreamingLocked() {
	if !a.streaming {
		return
	}
	close(a.stopChan)
	a.streaming = false

	if a.dev != nil {
		C.wrap_stop_streaming(a.dev)
	}

	// Restore the logical CH B state if SDK mode forced it on.
	if a.sdkForcedChB {
		a.applyChannelLocked(1, a.chB)
		a.sdkForcedChB = false
	}

	// Free pre-allocated buffers
	if a.streamBufA != nil {
		C.free(unsafe.Pointer(a.streamBufA))
		a.streamBufA = nil
	}
	if a.streamBufB != nil {
		C.free(unsafe.Pointer(a.streamBufB))
		a.streamBufB = nil
	}
}

func (a *App) IsStreaming() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.streaming
}

func (a *App) GetStreamStats() StreamStats {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected || !a.streaming {
		return StreamStats{}
	}

	var cStats C.ps_stream_stats_t
	C.wrap_get_streaming_stats(a.dev, &cStats)

	return StreamStats{
		Blocks:        uint64(cStats.blocks),
		TotalSamples:  uint64(cStats.total_samples),
		ElapsedS:      float64(cStats.elapsed_s),
		SamplesPerSec: float64(cStats.samples_per_sec),
		BlocksPerSec:  float64(cStats.blocks_per_sec),
		LastBlockMs:   float64(cStats.last_block_ms),
		Mode:          streamModeName(a.streamMode),
	}
}

// GetStreamingMode returns the active streaming mode ("fast"/"native")
// or empty string when not streaming.
func (a *App) GetStreamingMode() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.streaming {
		return ""
	}
	return streamModeName(a.streamMode)
}

// ExportCSV writes the supplied samples to a CSV file chosen by the user
// through a native save dialog. `dtNs` is the per-sample time spacing;
// `chB` may be nil for CH A only. Returns the full path on success.
func (a *App) ExportCSV(chA []float64, chB []float64, dtNs float64) (string, error) {
	path, err := wailsRuntime.SaveFileDialog(a.ctx, wailsRuntime.SaveDialogOptions{
		Title:           "Export waveform",
		DefaultFilename: fmt.Sprintf("picoscope_%d.csv", time.Now().Unix()),
		Filters: []wailsRuntime.FileFilter{
			{DisplayName: "CSV (*.csv)", Pattern: "*.csv"},
		},
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil // user cancelled
	}
	if !strings.HasSuffix(strings.ToLower(path), ".csv") {
		path += ".csv"
	}
	f, err := os.Create(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	if len(chB) > 0 {
		fmt.Fprintln(f, "t_ns,ch_a_mv,ch_b_mv")
	} else {
		fmt.Fprintln(f, "t_ns,ch_a_mv")
	}
	n := len(chA)
	if len(chB) > 0 && len(chB) < n {
		n = len(chB)
	}
	buf := &strings.Builder{}
	buf.Grow(n * 32)
	for i := 0; i < n; i++ {
		t := float64(i) * dtNs
		if len(chB) > 0 {
			fmt.Fprintf(buf, "%.1f,%.3f,%.3f\n", t, chA[i], chB[i])
		} else {
			fmt.Fprintf(buf, "%.1f,%.3f\n", t, chA[i])
		}
	}
	if _, err := f.WriteString(buf.String()); err != nil {
		return "", err
	}
	return path, nil
}

// ExportPNG writes a canvas screenshot (supplied as a base64-encoded
// `data:image/png;base64,...` URL from the frontend) to disk.
func (a *App) ExportPNG(dataUrl string) (string, error) {
	// Accept either "data:image/png;base64,XXX" or raw base64.
	const prefix = "base64,"
	idx := strings.Index(dataUrl, prefix)
	if idx >= 0 {
		dataUrl = dataUrl[idx+len(prefix):]
	}
	img, err := base64.StdEncoding.DecodeString(dataUrl)
	if err != nil {
		return "", fmt.Errorf("bad base64 image: %w", err)
	}
	path, err := wailsRuntime.SaveFileDialog(a.ctx, wailsRuntime.SaveDialogOptions{
		Title:           "Save screenshot",
		DefaultFilename: fmt.Sprintf("picoscope_%d.png", time.Now().Unix()),
		Filters: []wailsRuntime.FileFilter{
			{DisplayName: "PNG (*.png)", Pattern: "*.png"},
		},
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil
	}
	if !strings.HasSuffix(strings.ToLower(path), ".png") {
		path += ".png"
	}
	if err := os.WriteFile(path, img, 0644); err != nil {
		return "", err
	}
	return path, nil
}

/* ======================================================================== */
/* Signal Generator                                                         */
/* ======================================================================== */

var waveTypeMap = map[string]int{
	"sine": 0, "square": 1, "triangle": 2,
	"rampup": 3, "rampdown": 4, "dc": 5,
}

func (a *App) SetSiggen(waveType string, freqHz float64, pkpkMv float64) error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return fmt.Errorf("not connected")
	}

	wt, ok := waveTypeMap[waveType]
	if !ok {
		return fmt.Errorf("unknown wave type: %s", waveType)
	}

	// GUI passes amplitude in mV peak-to-peak; the C driver wants µV.
	if pkpkMv <= 0 {
		pkpkMv = 1000 // default 1 Vpp
	}
	pkpkUv := uint(pkpkMv * 1000)

	st := C.wrap_set_siggen(a.dev, C.int(wt), C.float(freqHz), C.uint(pkpkUv))
	if st != 0 {
		return fmt.Errorf("set_siggen failed (status=%d)", int(st))
	}
	return nil
}

// SetSiggenEx: full siggen with sweep, DC offset, duty cycle.
//   startHz/stopHz:  equal = fixed freq; different = sweep
//   incHz, dwellS:   sweep step + dwell time per step
//   pkpkMv:          peak-to-peak amplitude in mV (default 1000)
//   offsetMv:        DC offset in mV
//   dutyPct:         SQUARE duty cycle 0-100 (default 50)
func (a *App) SetSiggenEx(waveType string, startHz, stopHz, incHz, dwellS float64,
	pkpkMv, offsetMv float64, dutyPct int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	wt, ok := waveTypeMap[waveType]
	if !ok {
		return fmt.Errorf("unknown wave type: %s", waveType)
	}
	if pkpkMv <= 0 {
		pkpkMv = 1000
	}
	if dutyPct <= 0 || dutyPct > 100 {
		dutyPct = 50
	}
	st := C.wrap_set_siggen_ex(a.dev, C.int(wt),
		C.float(startHz), C.float(stopHz),
		C.float(incHz), C.float(dwellS),
		C.uint(pkpkMv*1000), C.int(offsetMv*1000), C.int(dutyPct))
	if st != 0 {
		return fmt.Errorf("set_siggen_ex failed (status=%d)", int(st))
	}
	return nil
}

// CalibrateDCOffset: assume the currently-enabled channels are at 0 V,
// measure their mean, store it as the range's offset correction.
func (a *App) CalibrateDCOffset() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	st := C.wrap_calibrate_dc_offset(a.dev)
	if st != 0 {
		return fmt.Errorf("calibrate_dc_offset failed (status=%d)", int(st))
	}
	return nil
}

// SetRangeCalibration: manually set the per-range (offset_mV, gain) used
// to post-process captured samples. Defaults are (0, 1) = identity.
var rangeNameToEnum = map[string]int{
	"50mV": 2, "100mV": 3, "200mV": 4, "500mV": 5,
	"1V": 6, "2V": 7, "5V": 8, "10V": 9, "20V": 10,
}

func (a *App) SetRangeCalibration(rangeName string, offsetMv, gain float64) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	r, ok := rangeNameToEnum[rangeName]
	if !ok {
		return fmt.Errorf("unknown range: %s", rangeName)
	}
	st := C.wrap_set_range_cal(a.dev, C.int(r), C.float(offsetMv), C.float(gain))
	if st != 0 {
		return fmt.Errorf("set_range_calibration failed (status=%d)", int(st))
	}
	return nil
}

// RangeCal holds the current (offset, gain) pair for a single range.
type RangeCal struct {
	Range    string  `json:"range"`
	OffsetMv float64 `json:"offset_mv"`
	Gain     float64 `json:"gain"`
}

// GetAllCalibration returns the current cal table (9 ranges). Used by the
// GUI's calibration modal to pre-fill the editor and export snapshots.
func (a *App) GetAllCalibration() ([]RangeCal, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return nil, fmt.Errorf("not connected")
	}
	out := make([]RangeCal, 0, 9)
	names := []string{"50mV", "100mV", "200mV", "500mV", "1V", "2V", "5V", "10V", "20V"}
	for _, n := range names {
		r := rangeNameToEnum[n]
		var off, g C.float
		if st := C.wrap_get_range_cal(a.dev, C.int(r), &off, &g); st != 0 {
			return nil, fmt.Errorf("get_range_cal %s failed (%d)", n, int(st))
		}
		out = append(out, RangeCal{Range: n, OffsetMv: float64(off), Gain: float64(g)})
	}
	return out, nil
}

// ApplyCalibration pushes a full table to the driver (from import or the
// calibration modal's "Apply" button). Silently skips unknown range names.
func (a *App) ApplyCalibration(entries []RangeCal) error {
	for _, e := range entries {
		if err := a.SetRangeCalibration(e.Range, e.OffsetMv, e.Gain); err != nil {
			return err
		}
	}
	return nil
}

func (a *App) DisableSiggen() error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected {
		return fmt.Errorf("not connected")
	}

	st := C.wrap_disable_siggen(a.dev)
	if st != 0 {
		return fmt.Errorf("disable_siggen failed (status=%d)", int(st))
	}
	return nil
}

/* ======================================================================== */
/* Measurements                                                             */
/* ======================================================================== */

func (a *App) ComputeMeasurements(data []float64) Measurements {
	if len(data) == 0 {
		return Measurements{}
	}
	minV := data[0]
	maxV := data[0]
	sum := 0.0
	for _, v := range data {
		if v < minV {
			minV = v
		}
		if v > maxV {
			maxV = v
		}
		sum += v
	}
	return Measurements{
		MinMv:  minV,
		MaxMv:  maxV,
		MeanMv: sum / float64(len(data)),
		VppMv:  maxV - minV,
	}
}

/* ======================================================================== */
/* Protocol decoder bindings                                                */
/* ======================================================================== */

// ListDecoders returns every decoder the frontend can drive.
func (a *App) ListDecoders() []decoder.Descriptor {
	return decoder.List()
}

// DecodeRequest is the wire-level shape the frontend sends. Keeping it
// separate from decoder.Context (which is not JSON-friendly for `any`)
// makes the Wails binding transparent.
type DecodeRequest struct {
	Protocol   string            `json:"protocol"`
	SamplesA   []float64         `json:"samplesA"`
	SamplesB   []float64         `json:"samplesB"`
	DtNs       float64           `json:"dtNs"`
	RangeMvA   float64           `json:"rangeMvA"`
	RangeMvB   float64           `json:"rangeMvB"`
	Config     map[string]any    `json:"config"`
	ChannelMap map[string]string `json:"channelMap"`
}

// StartDecoder spins up a stateful streaming decoder. Subsequent waveform
// blocks fed by the streaming goroutine will be decoded and emitted on
// the 'decoderEvents' Wails event. Calling it a second time replaces the
// active session (configuration change).
func (a *App) StartDecoder(protocol string, config map[string]any,
	channelMap map[string]string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.decSession != nil {
		a.decSession.Close()
	}
	a.decSession = decoder.NewSession(protocol, config, channelMap)
	return nil
}

// StopDecoder tears down the streaming decoder. Safe to call when no
// session is active.
func (a *App) StopDecoder() {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.decSession != nil {
		a.decSession.Close()
		a.decSession = nil
	}
}

// Decode runs one protocol-decoder pass and returns the Result. Any error
// (invalid protocol, bad dt_ns, etc.) is surfaced via Result.Error rather
// than a Go error — easier for the JS side to render uniformly.
func (a *App) Decode(req DecodeRequest) decoder.Result {
	ctx := decoder.Context{
		SamplesA:   req.SamplesA,
		SamplesB:   req.SamplesB,
		DtNs:       req.DtNs,
		RangeMvA:   req.RangeMvA,
		RangeMvB:   req.RangeMvB,
		Config:     req.Config,
		ChannelMap: req.ChannelMap,
	}
	return decoder.Decode(req.Protocol, ctx)
}
