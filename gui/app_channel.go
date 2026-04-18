package main

/*
#include "cgo_wrappers.h"
*/
import "C"

// applyChannelLocked pushes a channel config to the driver. Caller must
// hold a.mu.
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

func (a *App) GetChannelA() ChannelConfig { return a.chA }
func (a *App) GetChannelB() ChannelConfig { return a.chB }
