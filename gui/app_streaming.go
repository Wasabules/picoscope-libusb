package main

/*
#include "cgo_wrappers.h"
*/
import "C"

import (
	"fmt"
	"time"
	"unsafe"

	wailsRuntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

// StartStreaming starts streaming in the default (fast block) mode.
// Kept for backward compatibility with existing frontend code.
func (a *App) StartStreaming() error {
	return a.StartStreamingMode("fast")
}

// StartStreamingMode starts streaming with an explicit mode:
//   - "fast":   rapid block captures (~330 kS/s), supports dual channel
//   - "native": FPGA continuous mode (~100 S/s, CH A only, gap-free)
//   - "sdk":    SDK continuous mode (1 MS/s dual channel, gap-free) — use for UART
func (a *App) StartStreamingMode(mode string) error {
	a.mu.Lock()
	if !a.connected {
		a.mu.Unlock()
		return fmt.Errorf("not connected")
	}
	if a.streaming {
		a.mu.Unlock()
		return nil
	}

	modeInt := streamModeFast
	ringSize := ringFast
	switch mode {
	case "native":
		modeInt = streamModeNative
		ringSize = ringNative
	case "sdk":
		modeInt = streamModeSDK
		ringSize = ringSDK
	case "fast", "":
		modeInt = streamModeFast
		ringSize = ringFast
	default:
		a.mu.Unlock()
		return fmt.Errorf("unknown streaming mode %q", mode)
	}

	// PS_STREAM_SDK halves its hardware sample rate (500 kS/s instead of
	// 1 MS/s) when CH B is disabled in the gain bytes — the SDK trace was
	// captured with both channels on and the FPGA configures its ADC
	// pipeline accordingly. Force CH B on at the driver level for the
	// duration of the stream; chB.Enabled is preserved so stopStreaming can
	// restore the user-facing state.
	sdkForcedChB := false
	if modeInt == streamModeSDK && !a.chB.Enabled {
		rv, ok := rangeEnumVal[a.chA.Range]
		if !ok {
			rv = 8
		}
		C.wrap_set_channel(a.dev, 1, 1, 1, C.int(rv))
		sdkForcedChB = true
	}
	a.sdkForcedChB = sdkForcedChB

	st := C.wrap_start_streaming_mode(a.dev, C.int(modeInt), 1, C.int(ringSize))
	if st != 0 {
		if sdkForcedChB {
			a.applyChannelLocked(1, a.chB)
			a.sdkForcedChB = false
		}
		a.mu.Unlock()
		return fmt.Errorf("start_streaming failed (status=%d)", int(st))
	}

	// Pre-allocate C buffers. Native mode only delivers CH A.
	// Size: must match (or exceed) the C ring capacity so a stalled poll
	// can drain everything the hardware produced without truncating —
	// get_streaming_latest() keeps only the last N samples, so any cap
	// below the accumulated-since-last-drain count creates a hole at the
	// oldest end of the timeline. Fast mode produces ~781 kS/s; a 200 ms
	// GC pause or Wails IPC stall would accumulate ~156 k samples, which
	// blows the old 128 k cap and silently drops the earliest ~30 k.
	// Sizing at ringFast (1 M samples) eliminates that hole up to a full
	// 1.28 s stall. Memory impact: ~8 MB per channel.
	bufCap := ringFast
	if modeInt == streamModeSDK {
		bufCap = ringSDK
	}
	a.streamBufCap = bufCap
	a.streamLastSeen = 0
	a.streamBufA = (*C.float)(C.malloc(C.size_t(bufCap) * 4))
	if (modeInt == streamModeFast || modeInt == streamModeSDK) && a.chB.Enabled {
		a.streamBufB = (*C.float)(C.malloc(C.size_t(bufCap) * 4))
	}

	a.streaming = true
	a.streamMode = modeInt
	a.stopChan = make(chan struct{})
	a.mu.Unlock()

	go a.streamingPollLoop()
	return nil
}

func (a *App) streamingPollLoop() {
	// Poll interval depends on mode and timebase.
	//   Native  : hardware-limited to ~100 S/s, 200 ms is plenty.
	//   Fast tb≤10 : ~330 kS/s, 33 ms keeps the UI fluid.
	//   Fast tb≥11 : each block takes >80 ms (tb=11) up to minutes (tb=20+),
	//                so poll every 250 ms — anything faster just wastes CPU.
	a.mu.Lock()
	interval := 33 * time.Millisecond
	if a.streamMode == streamModeNative {
		interval = 200 * time.Millisecond
	} else if a.streamMode == streamModeSDK {
		// 1 MS/s ⇒ ~33k samples per 33ms poll. Keep UI fluid without burning
		// CPU; a faster poll gives no benefit because data streams gap-free.
		interval = 33 * time.Millisecond
	} else if a.timebase >= 11 {
		interval = 250 * time.Millisecond
	}
	bufCap := a.streamBufCap
	a.mu.Unlock()

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			a.mu.Lock()
			if !a.streaming || a.dev == nil {
				a.mu.Unlock()
				return
			}

			// Detect C-side thread death (fatal USB error, allocation
			// failure, etc.). In that case the driver cleared its
			// streaming flag internally; we need to tear down the Go
			// side and notify the frontend so the UI can re-enable
			// controls instead of appearing to stream forever.
			if C.wrap_is_streaming(a.dev) == 0 {
				a.stopStreamingLocked()
				a.mu.Unlock()
				wailsRuntime.EventsEmit(a.ctx, "streamStopped",
					map[string]string{"reason": "driver thread exited"})
				return
			}

			// Query stats first to learn how many samples the C ring has
			// accumulated total. `get_streaming_latest` returns the LAST N
			// entries in the ring — if we ask for fewer than the number of
			// samples written since last poll, we silently drop the oldest
			// ones. Use (total_now - last_seen) to pull exactly the new
			// region, capped at our buffer size.
			var cStats C.ps_stream_stats_t
			C.wrap_get_streaming_stats(a.dev, &cStats)

			totalNow := uint64(cStats.total_samples)
			newCount := uint64(0)
			if totalNow > a.streamLastSeen {
				newCount = totalNow - a.streamLastSeen
			}
			readN := int(newCount)
			if readN > bufCap {
				// Poll took too long and the ring has grown past our
				// buffer — we'll only get the most recent `bufCap` and
				// lose a bit of history. Rare unless the UI thread stalls.
				readN = bufCap
			}
			var actual C.int
			if readN > 0 {
				C.wrap_get_streaming_latest(a.dev, a.streamBufA, a.streamBufB,
					C.int(readN), &actual)
			}
			na := int(actual)
			a.streamLastSeen = totalNow

			var dataA, dataB []float64
			if na > 0 && a.chA.Enabled {
				dataA = cFloatsToGoSlice(a.streamBufA, na)
			}
			if na > 0 && (a.streamMode == streamModeFast || a.streamMode == streamModeSDK) &&
				a.chB.Enabled && a.streamBufB != nil {
				dataB = cFloatsToGoSlice(a.streamBufB, na)
			}

			tb := a.timebase
			chARng := a.chA.Range
			chBRng := a.chB.Range
			mode := a.streamMode
			a.mu.Unlock()

			if na > 0 {
				// Fast-streaming samples at a fixed ~1280 ns regardless of
				// the requested timebase (the cmd1 tb bytes are ignored by
				// the FPGA in that mode). Use the driver's observed rate —
				// ps2204a_timebase_to_ns(tb) would be off by 4× at tb=5 and
				// make a 1 kHz input look like 4 kHz on the trace.
				dtNs := int(C.wrap_streaming_dt_ns(a.dev))
				if dtNs <= 0 {
					dtNs = int(C.wrap_timebase_to_ns(C.int(tb)))
				}
				wailsRuntime.EventsEmit(a.ctx, "waveform", WaveformData{
					ChannelA:   dataA,
					ChannelB:   dataB,
					NumSamples: na,
					Timebase:   tb,
					TimebaseNs: dtNs,
					RangeMvA:   rangeMvVal[chARng],
					RangeMvB:   rangeMvVal[chBRng],
				})

				// Streaming decoder. Between two hardware capture blocks
				// the scope loses ~5.7 ms of signal, so samples either
				// side of the boundary are NOT contiguous on the wire
				// even though they arrive in one contiguous Go slice.
				// Slice the drained buffer at block_size boundaries and
				// feed the session one hardware block at a time, calling
				// ResetTail between blocks so the decoder never stitches
				// a UART frame across the real-signal gap (which was
				// producing phantom bytes at block seams).
				a.mu.Lock()
				sess := a.decSession
				a.mu.Unlock()
				if sess != nil {
					blockSize := maxSamplesSingle
					if a.chA.Enabled && a.chB.Enabled {
						blockSize = maxSamplesDual
					}
					if mode == streamModeNative || mode == streamModeSDK || blockSize <= 0 {
						// Native and SDK modes are truly gap-free — no
						// per-block seams, so feed as a continuous stream
						// and skip the ResetTail dance the fast-block path
						// needs between 8064-sample chunks.
						ev := sess.Feed(dataA, dataB, float64(dtNs),
							float64(rangeMvVal[chARng]),
							float64(rangeMvVal[chBRng]))
						if len(ev) > 0 {
							wailsRuntime.EventsEmit(a.ctx, "decoderEvents", ev)
						}
					} else {
						for off := 0; off < len(dataA); off += blockSize {
							end := off + blockSize
							if end > len(dataA) {
								end = len(dataA)
							}
							var chunkA []float64
							chunkA = dataA[off:end]
							var chunkB []float64
							if len(dataB) >= end {
								chunkB = dataB[off:end]
							}
							ev := sess.Feed(chunkA, chunkB, float64(dtNs),
								float64(rangeMvVal[chARng]),
								float64(rangeMvVal[chBRng]))
							if len(ev) > 0 {
								wailsRuntime.EventsEmit(a.ctx, "decoderEvents", ev)
							}
							// Drop cross-block history — the next chunk
							// is from a fresh capture and its first
							// samples are not adjacent in wire-time.
							sess.ResetTail()
						}
					}
				}
			}

			wailsRuntime.EventsEmit(a.ctx, "streamStats", StreamStats{
				Blocks:        uint64(cStats.blocks),
				TotalSamples:  uint64(cStats.total_samples),
				ElapsedS:      float64(cStats.elapsed_s),
				SamplesPerSec: float64(cStats.samples_per_sec),
				BlocksPerSec:  float64(cStats.blocks_per_sec),
				LastBlockMs:   float64(cStats.last_block_ms),
				Mode:          streamModeName(mode),
			})

		case <-a.stopChan:
			return
		}
	}
}

func streamModeName(m int) string {
	switch m {
	case streamModeNative:
		return "native"
	case streamModeSDK:
		return "sdk"
	default:
		return "fast"
	}
}

func (a *App) StopStreaming() error {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.streaming {
		return nil
	}
	a.stopStreamingLocked()
	return nil
}

func (a *App) stopStreamingLocked() {
	if !a.streaming {
		return
	}
	close(a.stopChan)
	a.streaming = false

	if a.dev != nil {
		C.wrap_stop_streaming(a.dev)
	}

	// Restore the logical CH B state if SDK mode forced it on.
	if a.sdkForcedChB {
		a.applyChannelLocked(1, a.chB)
		a.sdkForcedChB = false
	}

	// Free pre-allocated buffers
	if a.streamBufA != nil {
		C.free(unsafe.Pointer(a.streamBufA))
		a.streamBufA = nil
	}
	if a.streamBufB != nil {
		C.free(unsafe.Pointer(a.streamBufB))
		a.streamBufB = nil
	}
}

func (a *App) IsStreaming() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.streaming
}

func (a *App) GetStreamStats() StreamStats {
	a.mu.Lock()
	defer a.mu.Unlock()

	if !a.connected || !a.streaming {
		return StreamStats{}
	}

	var cStats C.ps_stream_stats_t
	C.wrap_get_streaming_stats(a.dev, &cStats)

	return StreamStats{
		Blocks:        uint64(cStats.blocks),
		TotalSamples:  uint64(cStats.total_samples),
		ElapsedS:      float64(cStats.elapsed_s),
		SamplesPerSec: float64(cStats.samples_per_sec),
		BlocksPerSec:  float64(cStats.blocks_per_sec),
		LastBlockMs:   float64(cStats.last_block_ms),
		Mode:          streamModeName(a.streamMode),
	}
}

// GetStreamingMode returns the active streaming mode ("fast"/"native"/"sdk")
// or empty string when not streaming.
func (a *App) GetStreamingMode() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.streaming {
		return ""
	}
	return streamModeName(a.streamMode)
}
