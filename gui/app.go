package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import (
	"context"
	"sync"

	"gui/decoder"
)

// App is the Wails-bound root type. Each method on *App is exposed to the
// frontend as a JS binding. Methods are split across app_*.go files by
// concern (connection, channel, timebase, trigger, capture, streaming,
// siggen, calibration, measurements, decoder, export) but all share the
// single `mu` mutex that guards the C device handle.
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
	streamMode int // streamModeFast / streamModeNative / streamModeSDK
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

// effectiveMaxSamples returns the maximum sample count for the current
// channel configuration.
func (a *App) effectiveMaxSamples() int {
	if a.chA.Enabled && a.chB.Enabled {
		return maxSamplesDual
	}
	return maxSamplesSingle
}
