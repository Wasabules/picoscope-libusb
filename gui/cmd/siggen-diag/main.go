// Headless siggen-during-streaming diagnostic harness.
//
// Runs two back-to-back experiments:
//   1. Block capture with siggen on — control sample (no streaming pressure
//      on EP 0x06, so siggen *must* work here if the hardware is OK).
//   2. SDK streaming start with siggen on — the mode that's currently broken.
//
// In both, we compute peak-to-peak and std on CH A (assumed looped back to
// the generator output; even stray pickup gives a measurable delta vs a
// fully silent DAC).
//
// Run from gui/:   go run ./cmd/siggen-diag -range 10V
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../../driver
#cgo LDFLAGS: ${SRCDIR}/../../../driver/libpicoscope2204a.a -lusb-1.0 -lpthread -lm
#include "picoscope2204a.h"
#include <stdlib.h>
*/
import "C"

import (
	"flag"
	"fmt"
	"log"
	"math"
	"time"
	"unsafe"
)

var rangeMv = map[string]struct {
	mv  int
	enm int
}{
	"50mV":  {50, 2},
	"100mV": {100, 3},
	"200mV": {200, 4},
	"500mV": {500, 5},
	"1V":    {1000, 6},
	"2V":    {2000, 7},
	"5V":    {3515, 8},
	"10V":   {9092, 9},
	"20V":   {20000, 10},
}

func main() {
	rangeFlag := flag.String("range", "20V", "CH A range (50mV..20V)")
	freq := flag.Float64("freq", 1000, "siggen frequency (Hz)")
	pkpk := flag.Uint("pkpk", 2000000, "siggen pk-pk µV")
	tb := flag.Int("tb", 5, "block capture timebase")
	duration := flag.Duration("dur", 500*time.Millisecond,
		"how long to stream per trial")
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	rng, ok := rangeMv[*rangeFlag]
	if !ok {
		log.Fatalf("unknown range %q", *rangeFlag)
	}
	log.Printf("[diag] CH A range = %s (%d mV)", *rangeFlag, rng.mv)

	var dev *C.ps2204a_device_t
	if st := C.ps2204a_open(&dev); st != 0 {
		log.Fatalf("ps2204a_open failed: status=%d", int(st))
	}
	defer C.ps2204a_close(dev)
	log.Printf("[diag] device opened")

	setCh := func(ch, rngEnum int) {
		if st := C.ps2204a_set_channel(dev, C.ps_channel_t(ch), C.bool(true),
			1, C.ps_range_t(rngEnum)); st != 0 {
			log.Fatalf("set_channel ch=%d: status=%d", ch, int(st))
		}
	}
	setCh(0, rng.enm) // A DC enabled
	setCh(1, rng.enm) // B enabled (SDK needs both for full 1 MS/s)

	if st := C.ps2204a_set_timebase(dev, C.int(*tb), 8064); st != 0 {
		log.Fatalf("set_timebase: status=%d", int(st))
	}
	if st := C.ps2204a_disable_trigger(dev); st != 0 {
		log.Fatalf("disable_trigger: status=%d", int(st))
	}

	// ---------------------------------------------------------------- Trial 1
	log.Printf("")
	log.Printf("[diag] === Trial 1: BLOCK capture + siggen ON (control) ===")
	applySiggen(dev, *freq, *pkpk, true)
	time.Sleep(150 * time.Millisecond)

	bufA := make([]float32, 8064)
	bufB := make([]float32, 8064)
	var actual C.int
	if st := C.ps2204a_capture_block(dev, 8064,
		(*C.float)(unsafe.Pointer(&bufA[0])),
		(*C.float)(unsafe.Pointer(&bufB[0])),
		&actual); st != 0 {
		log.Printf("[diag] capture_block failed: status=%d", int(st))
	} else {
		reportStats("block/A", bufA[:int(actual)])
		reportStats("block/B", bufB[:int(actual)])
	}
	applySiggen(dev, 0, 0, false)

	// ---------------------------------------------------------------- Trial 2
	log.Printf("")
	log.Printf("[diag] === Trial 2: SDK streaming + siggen ON (the target) ===")
	applySiggen(dev, *freq, *pkpk, true)
	time.Sleep(150 * time.Millisecond)

	ring := 1 << 22
	if st := C.ps2204a_start_streaming_mode(dev, 2 /*SDK*/, 1,
		nil, nil, C.int(ring)); st != 0 {
		log.Fatalf("start_streaming(SDK): status=%d", int(st))
	}
	time.Sleep(*duration)

	sampleN := 200_000
	sA := make([]float32, sampleN)
	sB := make([]float32, sampleN)
	if st := C.ps2204a_get_streaming_latest(dev,
		(*C.float)(unsafe.Pointer(&sA[0])),
		(*C.float)(unsafe.Pointer(&sB[0])),
		C.int(sampleN), &actual); st != 0 {
		log.Printf("[diag] get_streaming_latest: status=%d", int(st))
	}
	reportStats("sdk/A", sA[:int(actual)])
	reportStats("sdk/B", sB[:int(actual)])
	C.ps2204a_stop_streaming(dev)
	applySiggen(dev, 0, 0, false)

	// ---------------------------------------------------------------- Trial 3
	log.Printf("")
	log.Printf("[diag] === Trial 3: FAST streaming + siggen ON (known-good) ===")
	applySiggen(dev, *freq, *pkpk, true)
	time.Sleep(150 * time.Millisecond)
	if st := C.ps2204a_start_streaming_mode(dev, 0 /*FAST*/, 1,
		nil, nil, C.int(1<<20)); st != 0 {
		log.Fatalf("start_streaming(FAST): status=%d", int(st))
	}
	time.Sleep(*duration)
	if st := C.ps2204a_get_streaming_latest(dev,
		(*C.float)(unsafe.Pointer(&sA[0])),
		(*C.float)(unsafe.Pointer(&sB[0])),
		C.int(sampleN), &actual); st != 0 {
		log.Printf("[diag] get_streaming_latest: status=%d", int(st))
	}
	reportStats("fast/A", sA[:int(actual)])
	reportStats("fast/B", sB[:int(actual)])
	C.ps2204a_stop_streaming(dev)
	applySiggen(dev, 0, 0, false)

	log.Printf("")
	log.Printf("[diag] Interpretation:")
	log.Printf("  If block/A pk-pk ≈ fast/A pk-pk but sdk/A pk-pk is tiny,")
	log.Printf("  the DAC is silent during SDK streaming — my fix is not")
	log.Printf("  landing. If sdk/A matches, the generator works and the")
	log.Printf("  'no signal' symptom is elsewhere (probe / wiring / scale).")
}

func applySiggen(dev *C.ps2204a_device_t, hz float64, pkpkUv uint, on bool) {
	if !on {
		C.ps2204a_disable_siggen(dev)
		return
	}
	st := C.ps2204a_set_siggen(dev, 0 /*SINE*/, C.float(hz), C.uint32_t(pkpkUv))
	if st != 0 {
		log.Printf("[diag] set_siggen failed: status=%d", int(st))
	} else {
		log.Printf("[diag] siggen ON — sine %.1f Hz %d µVpp", hz, pkpkUv)
	}
}

func reportStats(label string, samples []float32) {
	if len(samples) == 0 {
		log.Printf("[diag] %-10s n=0 (no samples)", label)
		return
	}
	minV, maxV := float64(samples[0]), float64(samples[0])
	var sum, sumSq float64
	for _, v := range samples {
		x := float64(v)
		if x < minV {
			minV = x
		}
		if x > maxV {
			maxV = x
		}
		sum += x
		sumSq += x * x
	}
	n := float64(len(samples))
	mean := sum / n
	variance := sumSq/n - mean*mean
	if variance < 0 {
		variance = 0
	}
	std := math.Sqrt(variance)
	log.Printf("[diag] %-10s n=%6d  mean=%8.1f mV  pkpk=%8.1f mV  std=%7.2f mV  (min=%.1f max=%.1f)",
		label, len(samples), mean, maxV-minV, std, minV, maxV)
	_ = fmt.Sprint // avoid import purge
}
