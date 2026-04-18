package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import "fmt"

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
