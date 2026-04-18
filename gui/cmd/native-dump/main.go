// Native-streaming raw sample dumper.
//
// Opens the scope, starts PS_STREAM_NATIVE, and prints every block pulled
// from get_streaming_latest as a histogram + delta-between-samples summary.
// The goal is to see whether the "vertical lines at regular intervals" the
// user sees in the UI correspond to (a) packet header bytes appearing as
// samples, (b) repeated stale buffer content, or (c) something else.
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../../driver
#cgo LDFLAGS: ${SRCDIR}/../../../driver/libpicoscope2204a.a -lusb-1.0 -lpthread -lm
#include "picoscope2204a.h"
#include <stdlib.h>

static ps_status_t d_open(ps2204a_device_t **dev) { return ps2204a_open(dev); }
static void d_close(ps2204a_device_t *dev) { ps2204a_close(dev); }
static ps_status_t d_set_channel(ps2204a_device_t *dev, int ch, int en,
                                 int coupling, int rng) {
    return ps2204a_set_channel(dev, (ps_channel_t)ch, en,
                               (ps_coupling_t)coupling, (ps_range_t)rng);
}
static ps_status_t d_set_timebase(ps2204a_device_t *dev, int tb, int n) {
    return ps2204a_set_timebase(dev, tb, n);
}
static ps_status_t d_disable_trigger(ps2204a_device_t *dev) {
    return ps2204a_disable_trigger(dev);
}
static ps_status_t d_start_native(ps2204a_device_t *dev, int ring) {
    return ps2204a_start_streaming_mode(dev, PS_STREAM_NATIVE, 1, NULL, NULL, ring);
}
static ps_status_t d_stop(ps2204a_device_t *dev) {
    return ps2204a_stop_streaming(dev);
}
static ps_status_t d_get_latest(ps2204a_device_t *dev, float *a, int n, int *actual) {
    return ps2204a_get_streaming_latest(dev, a, NULL, n, actual);
}
static int d_dt_ns(ps2204a_device_t *dev) {
    return ps2204a_get_streaming_dt_ns(dev);
}
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

func main() {
	duration := flag.Duration("duration", 5*time.Second, "streaming duration")
	poll := flag.Duration("poll", 200*time.Millisecond, "poll interval")
	flag.Parse()
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	var dev *C.ps2204a_device_t
	if st := C.d_open(&dev); st != 0 {
		log.Fatalf("open: status=%d", int(st))
	}
	defer C.d_close(dev)

	// 10V single channel, DC coupled.
	C.d_set_channel(dev, 0, 1, 1, 9)
	C.d_set_channel(dev, 1, 0, 1, 9)
	C.d_set_timebase(dev, 5, 8064)
	C.d_disable_trigger(dev)

	if st := C.d_start_native(dev, 1<<20); st != 0 {
		log.Fatalf("start_native: status=%d", int(st))
	}
	defer C.d_stop(dev)

	log.Printf("native streaming started, dt=%d ns", int(C.d_dt_ns(dev)))

	const N = 4096
	buf := (*C.float)(C.malloc(C.size_t(N) * 4))
	defer C.free(unsafe.Pointer(buf))

	deadline := time.Now().Add(*duration)
	ticker := time.NewTicker(*poll)
	defer ticker.Stop()

	prevEnd := float64(0)
	havePrevEnd := false
	total := 0

	for now := range ticker.C {
		if now.After(deadline) {
			break
		}
		var actual C.int
		C.d_get_latest(dev, buf, C.int(N), &actual)
		n := int(actual)
		if n == 0 {
			log.Printf("poll: 0 samples")
			continue
		}
		src := (*[1 << 30]C.float)(unsafe.Pointer(buf))[:n:n]

		// Stats over the whole returned window.
		mn, mx := float64(src[0]), float64(src[0])
		var sum, sumsq float64
		for i := 0; i < n; i++ {
			v := float64(src[i])
			if v < mn {
				mn = v
			}
			if v > mx {
				mx = v
			}
			sum += v
			sumsq += v * v
		}
		mean := sum / float64(n)
		std := math.Sqrt(sumsq/float64(n) - mean*mean)

		// Sample-to-sample deltas — large spikes reveal discontinuities
		// that would render as vertical lines on the canvas.
		maxJump := 0.0
		jumpIdx := -1
		var spikes []int
		for i := 1; i < n; i++ {
			d := math.Abs(float64(src[i]) - float64(src[i-1]))
			if d > maxJump {
				maxJump = d
				jumpIdx = i
			}
			if d > 1000 {
				spikes = append(spikes, i)
			}
		}
		if len(spikes) > 0 {
			log.Printf("  SPIKES (>1000 mV jump) at %d positions: %v",
				len(spikes), spikes)
		}

		// Reprint of end-of-buffer samples — if the buffer is stale and we
		// re-read it, the first few samples of *this* poll will equal the
		// last sample of the previous poll exactly.
		endMatch := ""
		if havePrevEnd && float64(src[0]) == prevEnd {
			endMatch = " [starts at prev-end — stale ring]"
		}
		prevEnd = float64(src[n-1])
		havePrevEnd = true

		total += n
		log.Printf("n=%d total=%d min=%6.0f max=%6.0f mean=%6.0f std=%5.1f maxJump=%6.0f@%d%s",
			n, total, mn, mx, mean, std, maxJump, jumpIdx, endMatch)

		if n >= 20 {
			// Head and tail dump.
			head := ""
			for i := 0; i < 10; i++ {
				head += fmt.Sprintf("%5.0f ", float64(src[i]))
			}
			tail := ""
			for i := n - 10; i < n; i++ {
				tail += fmt.Sprintf("%5.0f ", float64(src[i]))
			}
			log.Printf("  head: %s", head)
			log.Printf("  tail: %s", tail)
		}
	}
}
