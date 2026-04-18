package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import "fmt"

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
