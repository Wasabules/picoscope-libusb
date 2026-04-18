package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import "fmt"

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
