package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import (
	"fmt"
	"unsafe"
)

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
