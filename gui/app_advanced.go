package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import (
	"fmt"
	"unsafe"
)

// Driver capabilities that had no binding: resolution enhancement, overflow
// reporting, equivalent-time sampling, window / pulse-width triggers,
// arbitrary waveforms, aggregated streaming reads and the per-unit EEPROM
// calibration table.

// ---------------------------------------------------------------- resolution

// SetResolutionEnhancement trades bandwidth for vertical resolution by
// averaging 4^extraBits neighbouring samples. Each extra bit halves the noise
// on this hardware: measured 6.14 -> 9.44 effective bits at extraBits=4.
// extraBits is 0 (off) to 4.
func (a *App) SetResolutionEnhancement(extraBits int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	if extraBits < 0 || extraBits > 4 {
		return fmt.Errorf("extra_bits must be 0..4, got %d", extraBits)
	}
	if st := C.wrap_set_resolution_enhancement(a.dev, C.int(extraBits)); st != 0 {
		return fmt.Errorf("set_resolution_enhancement failed (status=%d)", int(st))
	}
	a.resExtraBits = extraBits
	return nil
}

func (a *App) GetResolutionEnhancement() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.resExtraBits
}

// ----------------------------------------------------------------- overflow

// Overflow reports how many samples of the last capture sat on an ADC rail.
// The driver clamps corrected values to ±range, which otherwise hides the
// difference between a signal at full scale and one driven past it.
type Overflow struct {
	ClippedA int  `json:"clippedA"`
	ClippedB int  `json:"clippedB"`
	Total    int  `json:"total"`
	OverA    bool `json:"overA"`
	OverB    bool `json:"overB"`
}

func (a *App) GetLastOverflow() (Overflow, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return Overflow{}, fmt.Errorf("not connected")
	}
	var ca, cb, tot C.uint
	if st := C.wrap_get_last_overflow(a.dev, &ca, &cb, &tot); st != 0 {
		return Overflow{}, fmt.Errorf("get_last_overflow failed (status=%d)", int(st))
	}
	return Overflow{
		ClippedA: int(ca), ClippedB: int(cb), Total: int(tot),
		OverA: ca > 0, OverB: cb > 0,
	}, nil
}

// ---------------------------------------------------------------------- ETS

// EtsResult carries what an ETS capture produced: the interleaved samples and
// the effective per-sample interval, which is far finer than the timebase.
type EtsResult struct {
	ChannelA   []float64 `json:"channelA"`
	ChannelB   []float64 `json:"channelB"`
	NumSamples int       `json:"numSamples"`
	IntervalPs int       `json:"intervalPs"`
}

// SetEts enables equivalent-time sampling. Repetitive signals are rebuilt from
// many triggered captures, each offset by a fraction of a sample, so the
// effective rate becomes interleaves × the real one — 1 GS/s in fast mode,
// 2 GS/s in slow mode, against 100 MS/s live.
// mode: 0 = off, 1 = fast, 2 = slow. Pass 0 for interleaves/cycles to take the
// mode's defaults. Returns the effective interval in picoseconds.
func (a *App) SetEts(mode, interleaves, cycles int) (int, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return 0, fmt.Errorf("not connected")
	}
	var ips C.int
	st := C.wrap_set_ets(a.dev, C.int(mode), C.int(interleaves), C.int(cycles), &ips)
	if st != 0 {
		return 0, fmt.Errorf("set_ets failed (status=%d)", int(st))
	}
	a.etsMode = mode
	a.etsIntervalPs = int(ips)
	return int(ips), nil
}

func (a *App) DisableEts() error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	if st := C.wrap_disable_ets(a.dev); st != 0 {
		return fmt.Errorf("disable_ets failed (status=%d)", int(st))
	}
	a.etsMode = 0
	a.etsIntervalPs = 0
	return nil
}

// CaptureEts runs an ETS acquisition. nSamples is the per-cycle count; the
// result holds up to nSamples × interleaves points.
func (a *App) CaptureEts(nSamples int) (EtsResult, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return EtsResult{}, fmt.Errorf("not connected")
	}
	if a.etsMode == 0 {
		return EtsResult{}, fmt.Errorf("ETS is not enabled")
	}
	if nSamples < 1 || nSamples > 8192 {
		return EtsResult{}, fmt.Errorf("n_samples must be 1..8192, got %d", nSamples)
	}
	// Worst case is nSamples × 20 interleaves.
	capN := nSamples * 20
	bufA := make([]C.float, capN)
	bufB := make([]C.float, capN)
	var actual, intervalPs C.int

	st := C.wrap_capture_ets(a.dev, C.int(nSamples),
		(*C.float)(unsafe.Pointer(&bufA[0])),
		(*C.float)(unsafe.Pointer(&bufB[0])),
		C.int(capN), &actual, &intervalPs)
	if st != 0 {
		return EtsResult{}, fmt.Errorf("capture_ets failed (status=%d)", int(st))
	}

	n := int(actual)
	res := EtsResult{NumSamples: n, IntervalPs: int(intervalPs)}
	if a.chA.Enabled {
		res.ChannelA = make([]float64, n)
		for i := 0; i < n; i++ {
			res.ChannelA[i] = float64(bufA[i])
		}
	}
	if a.chB.Enabled {
		res.ChannelB = make([]float64, n)
		for i := 0; i < n; i++ {
			res.ChannelB[i] = float64(bufB[i])
		}
	}
	return res, nil
}

// ---------------------------------------------------------------- triggers

// SetTriggerEx is SetTrigger plus a hysteresis band, in ADC codes, which stops
// a noisy edge from re-triggering on its own ripple.
func (a *App) SetTriggerEx(source string, thresholdMv float64, direction string,
	delayPct float64, autoMs, hysteresisCounts int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	src, dir, err := triggerSourceDir(source, direction)
	if err != nil {
		return err
	}
	st := C.wrap_set_trigger_ex(a.dev, C.int(src), C.float(thresholdMv), C.int(dir),
		C.float(delayPct), C.int(autoMs), C.int(hysteresisCounts))
	if st != 0 {
		return fmt.Errorf("set_trigger_ex failed (status=%d)", int(st))
	}
	return nil
}

// SetTriggerWindow fires when the signal enters or leaves a voltage band —
// the usual way to catch a level that drifts out of tolerance.
func (a *App) SetTriggerWindow(source string, lowerMv, upperMv float64,
	direction string, delayPct float64, autoMs int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	if upperMv <= lowerMv {
		return fmt.Errorf("upper (%.1f) must exceed lower (%.1f)", upperMv, lowerMv)
	}
	src, dir, err := triggerSourceDir(source, direction)
	if err != nil {
		return err
	}
	st := C.wrap_set_trigger_window(a.dev, C.int(src), C.float(lowerMv),
		C.float(upperMv), C.int(dir), C.float(delayPct), C.int(autoMs))
	if st != 0 {
		return fmt.Errorf("set_trigger_window failed (status=%d)", int(st))
	}
	return nil
}

// SetTriggerPwq fires on a level crossing only when the pulse that preceded it
// lasted between lowerNs and upperNs — for hunting runts and glitches.
func (a *App) SetTriggerPwq(source string, thresholdMv float64, direction string,
	lowerNs, upperNs int, delayPct float64, autoMs int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	if upperNs > 0 && upperNs <= lowerNs {
		return fmt.Errorf("upper_ns (%d) must exceed lower_ns (%d)", upperNs, lowerNs)
	}
	src, dir, err := triggerSourceDir(source, direction)
	if err != nil {
		return err
	}
	st := C.wrap_set_trigger_pwq(a.dev, C.int(src), C.float(thresholdMv), C.int(dir),
		C.int(lowerNs), C.int(upperNs), C.float(delayPct), C.int(autoMs))
	if st != 0 {
		return fmt.Errorf("set_trigger_pwq failed (status=%d)", int(st))
	}
	return nil
}

func triggerSourceDir(source, direction string) (int, int, error) {
	src := 0
	switch source {
	case "A", "a":
		src = 0
	case "B", "b":
		src = 1
	default:
		return 0, 0, fmt.Errorf("unknown trigger source: %s", source)
	}
	dir := 0
	switch direction {
	case "rising":
		dir = 0
	case "falling":
		dir = 1
	default:
		return 0, 0, fmt.Errorf("unknown trigger direction: %s", direction)
	}
	return src, dir, nil
}

// -------------------------------------------------------- arbitrary waveform

// SetSiggenArbitrary uploads a user-defined waveform. samples are normalised
// to -1.0..+1.0 and resampled to the device LUT; up to 4096 points.
func (a *App) SetSiggenArbitrary(samples []float64, frequencyHz float64, pkpkMv int) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	if len(samples) < 2 || len(samples) > 4096 {
		return fmt.Errorf("need 2..4096 samples, got %d", len(samples))
	}
	lut := make([]C.short, len(samples))
	for i, v := range samples {
		if v > 1 {
			v = 1
		} else if v < -1 {
			v = -1
		}
		lut[i] = C.short(v * 32767.0)
	}
	st := C.wrap_set_siggen_arbitrary(a.dev, (*C.short)(unsafe.Pointer(&lut[0])),
		C.int(len(lut)), C.float(frequencyHz), C.uint(pkpkMv*1000))
	if st != 0 {
		return fmt.Errorf("set_siggen_arbitrary failed (status=%d)", int(st))
	}
	return nil
}

// ------------------------------------------------------ aggregated streaming

// AggregatedTrace is a (min, max) envelope per bucket.
type AggregatedTrace struct {
	MinA    []float64 `json:"minA"`
	MaxA    []float64 `json:"maxA"`
	MinB    []float64 `json:"minB"`
	MaxB    []float64 `json:"maxB"`
	Buckets int       `json:"buckets"`
}

// GetStreamingAggregated reduces the ring to nBuckets (min, max) pairs.
// Decimating instead drops whatever falls between the samples it keeps, so a
// narrow glitch in a multi-second window simply disappears; keeping both
// extremes of each bucket bounds the signal rather than sampling it.
// span = 0 means "everything the ring holds".
func (a *App) GetStreamingAggregated(nBuckets, span int) (AggregatedTrace, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return AggregatedTrace{}, fmt.Errorf("not connected")
	}
	if nBuckets < 1 || nBuckets > 65536 {
		return AggregatedTrace{}, fmt.Errorf("n_buckets must be 1..65536, got %d", nBuckets)
	}
	minA := make([]C.float, nBuckets)
	maxA := make([]C.float, nBuckets)
	minB := make([]C.float, nBuckets)
	maxB := make([]C.float, nBuckets)
	var actual C.int

	st := C.wrap_get_streaming_aggregated(a.dev,
		(*C.float)(unsafe.Pointer(&minA[0])), (*C.float)(unsafe.Pointer(&maxA[0])),
		(*C.float)(unsafe.Pointer(&minB[0])), (*C.float)(unsafe.Pointer(&maxB[0])),
		C.int(nBuckets), C.int(span), &actual)
	if st != 0 {
		return AggregatedTrace{}, fmt.Errorf("get_streaming_aggregated failed (status=%d)", int(st))
	}

	n := int(actual)
	out := AggregatedTrace{Buckets: n}
	conv := func(src []C.float) []float64 {
		d := make([]float64, n)
		for i := 0; i < n; i++ {
			d[i] = float64(src[i])
		}
		return d
	}
	if a.chA.Enabled {
		out.MinA, out.MaxA = conv(minA), conv(maxA)
	}
	if a.chB.Enabled {
		out.MinB, out.MaxB = conv(minB), conv(maxB)
	}
	return out, nil
}

// ------------------------------------------------------- EEPROM calibration

// EepromCal is the per-unit factory trim stored on the device itself.
type EepromCal struct {
	Valid    bool      `json:"valid"`
	Active   bool      `json:"active"`
	Ranges   []string  `json:"ranges"`
	OffsetMv []float64 `json:"offsetMv"`
	GainA    []float64 `json:"gainA"`
	GainB    []float64 `json:"gainB"`
}

// GetEepromCalibration returns the table decoded from the device EEPROM.
//
// The offsets are confirmed against a hand-measured reference (Pearson
// r = 0.9989). The gain blocks are not: measuring one signal across four
// ranges — which the correct table must render identically — was worse with
// them than with the built-in table, so the built-in one stays the default and
// this is opt-in. See docs/protocol.md.
func (a *App) GetEepromCalibration() (EepromCal, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return EepromCal{}, fmt.Errorf("not connected")
	}
	const n = 9
	var valid C.int
	off := make([]C.float, n)
	ga := make([]C.float, n)
	gb := make([]C.float, n)
	st := C.wrap_get_eeprom_cal(a.dev, &valid,
		(*C.float)(unsafe.Pointer(&off[0])),
		(*C.float)(unsafe.Pointer(&ga[0])),
		(*C.float)(unsafe.Pointer(&gb[0])))
	if st != 0 {
		return EepromCal{}, fmt.Errorf("get_eeprom_calibration failed (status=%d)", int(st))
	}
	out := EepromCal{
		Valid:  valid != 0,
		Active: C.wrap_eeprom_cal_active(a.dev) != 0,
		Ranges: []string{"50mV", "100mV", "200mV", "500mV", "1V", "2V", "5V", "10V", "20V"},
	}
	for i := 0; i < n; i++ {
		out.OffsetMv = append(out.OffsetMv, float64(off[i]))
		out.GainA = append(out.GainA, float64(ga[i]))
		out.GainB = append(out.GainB, float64(gb[i]))
	}
	return out, nil
}

// UseEepromCalibration switches between the device's own trim and the
// built-in reference table.
func (a *App) UseEepromCalibration(enable bool) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.connected {
		return fmt.Errorf("not connected")
	}
	v := C.int(0)
	if enable {
		v = 1
	}
	if st := C.wrap_use_eeprom_cal(a.dev, v); st != 0 {
		return fmt.Errorf("use_eeprom_calibration failed (status=%d)", int(st))
	}
	return nil
}
