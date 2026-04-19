// Headless UART diagnostic harness.
//
// Talks directly to the C driver (libpicoscope2204a.a) the same way the Wails
// app does, feeds streaming blocks into a decoder.Session, and runs
// uart_send.py in parallel. Every decoder log line is mirrored to
// stdout so we can diagnose without any UI.
//
// Usage:
//
//	go run ./cmd/uart-diag -range 10V -tb 5 -baud 9600 -text Hello -duration 3s
//
// Must be run from the gui/ directory (cgo resolves ../driver relative to
// SRCDIR).
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../../../driver
#cgo LDFLAGS: ${SRCDIR}/../../../driver/libpicoscope2204a.a -lusb-1.0 -lpthread -lm
#include "picoscope2204a.h"
#include <stdlib.h>

static ps_status_t d_open(ps2204a_device_t **dev) {
    return ps2204a_open(dev);
}
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
static ps_status_t d_start_stream(ps2204a_device_t *dev, int mode,
                                  int interval_us, int ring) {
    return ps2204a_start_streaming_mode(dev, (ps_stream_mode_t)mode,
                                        interval_us, NULL, NULL, ring);
}
static ps_status_t d_stop_stream(ps2204a_device_t *dev) {
    return ps2204a_stop_streaming(dev);
}
static int d_is_streaming(ps2204a_device_t *dev) {
    return ps2204a_is_streaming(dev) ? 1 : 0;
}
static ps_status_t d_get_latest(ps2204a_device_t *dev, float *a, float *b,
                                int n, int *actual) {
    return ps2204a_get_streaming_latest(dev, a, b, n, actual);
}
static int d_tb_to_ns(int tb) { return ps2204a_timebase_to_ns(tb); }
static int d_stream_dt_ns(ps2204a_device_t *dev) {
    return ps2204a_get_streaming_dt_ns(dev);
}
static unsigned long long d_stream_total(ps2204a_device_t *dev) {
    ps_stream_stats_t s;
    if (ps2204a_get_streaming_stats(dev, &s) != PS_OK) return 0;
    return (unsigned long long)s.total_samples;
}
static ps_status_t d_set_sdk_interval_ns(ps2204a_device_t *dev, unsigned int ns) {
    return ps2204a_set_sdk_stream_interval_ns(dev, (uint32_t)ns);
}
static ps_status_t d_set_sdk_auto_stop(ps2204a_device_t *dev,
                                       unsigned long long max_samples) {
    return ps2204a_set_sdk_stream_auto_stop(dev, (uint64_t)max_samples);
}
*/
import "C"

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"os/signal"
	"syscall"
	"time"
	"unsafe"

	"gui/decoder"
)

// Must mirror app.go rangeMvVal.
var rangeMv = map[string]struct {
	mv  int
	enm int // ps_range_t
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
	var (
		rangeFlag = flag.String("range", "10V", "range (50mV|100mV|…|20V)")
		tb        = flag.Int("tb", 5, "timebase (0..10 for fast streaming)")
		baud      = flag.Int("baud", 9600, "decoder-side baud")
		senderBaud = flag.Int("sender-baud", 0,
			"baud value passed to pyserial (0 = use -baud)")
		text      = flag.String("text", "Hello", "payload for uart_send.py")
		count     = flag.Int("count", 5, "how many times to send the payload")
		rate      = flag.Float64("rate", 0, "payloads/s (0 = as fast as possible)")
		loop      = flag.Bool("loop", false, "send continuously for the whole duration")
		port      = flag.String("port", "/dev/ttyUSB0", "serial device")
		duration  = flag.Duration("duration", 3*time.Second,
			"how long to stream after sending finishes")
		sendAfter = flag.Duration("send-after", 500*time.Millisecond,
			"delay before starting the UART sender")
		mode = flag.Int("mode", 0, "0=fast, 1=native, 2=sdk (gap-free 1 MS/s)")
		sdkIntervalNs = flag.Int("sdk-interval-ns", 0,
			"SDK-mode per-sample interval in ns (0=1 µs default, 500..1000000, multiples of 10)")
		sdkMaxSamples = flag.Int64("sdk-max-samples", 0,
			"SDK-mode client-side auto-stop cap (0=free-running)")
	)
	flag.Parse()

	log.SetFlags(log.LstdFlags | log.Lmicroseconds)

	rng, ok := rangeMv[*rangeFlag]
	if !ok {
		log.Fatalf("unknown range %q", *rangeFlag)
	}

	var dev *C.ps2204a_device_t
	if st := C.d_open(&dev); st != 0 {
		log.Fatalf("ps2204a_open failed: status=%d", int(st))
	}
	defer C.d_close(dev)
	log.Printf("[diag] device opened")

	// CH A only (DATA), DC-coupled, at the requested range.
	if st := C.d_set_channel(dev, 0, 1, 1, C.int(rng.enm)); st != 0 {
		log.Fatalf("set_channel A: status=%d", int(st))
	}
	// CH B enabled even though we don't read it: PS_STREAM_SDK's dual-ADC
	// hardware path halves its sample rate when CH B is off (500 kS/s instead
	// of 1 MS/s). Keeping B enabled forces the full 1 MS/s capture.
	if st := C.d_set_channel(dev, 1, 1, 1, C.int(rng.enm)); st != 0 {
		log.Fatalf("set_channel B: status=%d", int(st))
	}
	if st := C.d_set_timebase(dev, C.int(*tb), 8064); st != 0 {
		log.Fatalf("set_timebase: status=%d", int(st))
	}
	if st := C.d_disable_trigger(dev); st != 0 {
		log.Fatalf("disable_trigger: status=%d", int(st))
	}
	tbDtNs := float64(C.d_tb_to_ns(C.int(*tb)))
	log.Printf("[diag] range=%s (%d mV) tb=%d formula_dt=%.0f ns baud=%d",
		*rangeFlag, rng.mv, *tb, tbDtNs, *baud)

	ring := 1 << 20
	if *mode == 2 { // SDK streaming at 1 MS/s needs a bigger ring
		ring = 1 << 22 // 4M samples ≈ 4 s of history
		if *sdkIntervalNs != 0 {
			if st := C.d_set_sdk_interval_ns(dev, C.uint(*sdkIntervalNs)); st != 0 {
				log.Fatalf("set_sdk_stream_interval_ns(%d): status=%d",
					*sdkIntervalNs, int(st))
			}
			log.Printf("[diag] sdk interval set to %d ns", *sdkIntervalNs)
		}
		if *sdkMaxSamples != 0 {
			if st := C.d_set_sdk_auto_stop(dev, C.ulonglong(*sdkMaxSamples)); st != 0 {
				log.Fatalf("set_sdk_stream_auto_stop(%d): status=%d",
					*sdkMaxSamples, int(st))
			}
			log.Printf("[diag] sdk auto-stop armed at %d samples", *sdkMaxSamples)
		}
	}
	if st := C.d_start_stream(dev, C.int(*mode), 1, C.int(ring)); st != 0 {
		log.Fatalf("start_streaming: status=%d", int(st))
	}
	defer C.d_stop_stream(dev)

	// Actual per-sample rate — in PS_STREAM_FAST this is the hardware-fixed
	// ~1280 ns, not what the formula predicts. Using tbDtNs here (what the
	// decoder used to be fed) is the root cause of the 4× bit-period error
	// the UART decoder was hitting.
	dtNs := float64(C.d_stream_dt_ns(dev))
	if dtNs <= 0 {
		dtNs = tbDtNs
	}
	log.Printf("[diag] actual stream dt=%.0f ns (hw-measured)", dtNs)

	// Open a decoder session exactly like the Wails app.
	cfg := map[string]any{
		"baud":          float64(*baud),
		"dataBits":      float64(8),
		"parity":        "none",
		"stopBits":      float64(1),
		"lsbFirst":      true,
		"autoThreshold": true,
		"thresholdMv":   1500.0,
	}
	chMap := map[string]string{"DATA": "A"}
	sess := decoder.NewSession("uart", cfg, chMap)
	defer sess.Close()

	// Allocate the C-side read buffer (float32 per sample). Large enough to
	// drain every sample the ring accumulates between polls — ring grows
	// ~10-16 k samples per 33 ms poll at Fast mode, ~33 k at SDK 1 MS/s, so
	// 256 k covers bursts in both modes with headroom for GC stalls.
	const cap = 256 * 1024
	bufA := (*C.float)(C.malloc(C.size_t(cap) * 4))
	defer C.free(unsafe.Pointer(bufA))
	var lastSeen uint64

	// Ctrl-C handling.
	stop := make(chan struct{})
	sigc := make(chan os.Signal, 1)
	signal.Notify(sigc, syscall.SIGINT, syscall.SIGTERM)
	go func() { <-sigc; close(stop) }()

	// Launch the sender after a short delay so the scope is already
	// streaming before the bytes hit the wire.
	senderCtx, senderCancel := context.WithCancel(context.Background())
	defer senderCancel()
	senderDone := make(chan struct{})
	go func() {
		defer close(senderDone)
		select {
		case <-time.After(*sendAfter):
		case <-senderCtx.Done():
			return
		}
		script := findSenderScript()
		sb := *senderBaud
		if sb == 0 {
			sb = *baud
		}
		args := []string{
			script,
			"--port", *port,
			"--baud", fmt.Sprint(sb),
			"--text", *text,
		}
		if *loop {
			args = append(args, "--loop")
			if *rate > 0 {
				args = append(args, "--rate", fmt.Sprintf("%g", *rate))
			}
		} else {
			args = append(args, "--count", fmt.Sprint(*count))
			if *rate > 0 {
				args = append(args, "--rate", fmt.Sprintf("%g", *rate))
			}
		}
		log.Printf("[diag] launching sender: python3 %v", args)
		cmd := exec.CommandContext(senderCtx, "python3", args...)
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil && senderCtx.Err() == nil {
			log.Printf("[diag] sender error: %v", err)
		}
	}()

	// Poll loop mirroring app.go streamingPollLoop.
	ticker := time.NewTicker(33 * time.Millisecond)
	defer ticker.Stop()
	deadline := time.Now().Add(*duration + *sendAfter)
	totalEvents := 0

	for {
		select {
		case <-stop:
			log.Printf("[diag] interrupted")
			printSummary(totalEvents)
			return
		case <-ticker.C:
			if C.d_is_streaming(dev) == 0 {
				log.Printf("[diag] driver stopped streaming unexpectedly")
				printSummary(totalEvents)
				return
			}
			totalNow := uint64(C.d_stream_total(dev))
			newCount := uint64(0)
			if totalNow > lastSeen {
				newCount = totalNow - lastSeen
			}
			readN := int(newCount)
			if readN > cap {
				readN = cap
			}
			var actual C.int
			if readN > 0 {
				C.d_get_latest(dev, bufA, nil, C.int(readN), &actual)
			}
			na := int(actual)
			lastSeen = totalNow
			if na > 0 {
				dataA := make([]float64, na)
				src := (*[1 << 30]C.float)(unsafe.Pointer(bufA))[:na:na]
				for i := 0; i < na; i++ {
					dataA[i] = float64(src[i])
				}
				// Slice at hardware block boundaries — see App.go comment
				// on the equivalent feed loop for the full rationale.
				blockSize := 8064
				if *mode != 0 {
					// native & sdk: gap-free continuous stream, no seams
					blockSize = len(dataA)
				}
				for off := 0; off < len(dataA); off += blockSize {
					end := off + blockSize
					if end > len(dataA) {
						end = len(dataA)
					}
					events := sess.Feed(dataA[off:end], nil, dtNs,
						float64(rng.mv), float64(rng.mv))
					for _, e := range events {
						totalEvents++
						log.Printf("[diag.event] t=%.3f ms  %s  %q",
							e.TNs/1e6, e.Annotation, e.Text)
					}
					sess.ResetTail()
				}
			}
			if time.Now().After(deadline) {
				select {
				case <-senderDone:
				default:
				}
				log.Printf("[diag] duration elapsed")
				printSummary(totalEvents)
				return
			}
		}
	}
}

func printSummary(n int) {
	log.Printf("[diag] === %d total decoded events ===", n)
}

func findSenderScript() string {
	for _, p := range []string{
		"../python/examples/uart_send.py",
		"../../python/examples/uart_send.py",
		"../../../python/examples/uart_send.py",
	} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return "python/examples/uart_send.py"
}
