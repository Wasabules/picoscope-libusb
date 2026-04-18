package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import "fmt"

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
