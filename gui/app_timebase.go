package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import "fmt"

// clampSamplesLocked ensures the current sample count is valid for the
// current channel config; reapplies timebase if it changes. Caller must
// hold a.mu.
func (a *App) clampSamplesLocked() {
	m := a.effectiveMaxSamples()
	if a.samples > m {
		a.samples = m
		if a.connected {
			C.wrap_set_timebase(a.dev, C.int(a.timebase), C.int(a.samples))
		}
	}
}

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
