package decoder

import (
	"log"
	"sync"
)

// Session is a stateful, streaming decoder. The streaming goroutine feeds
// it raw sample blocks as they arrive; the session concatenates each block
// with a small tail from the previous call so frames that straddle block
// boundaries are still decoded, deduplicates events that fall inside the
// tail (already emitted), and returns fresh events timestamped from
// absolute session start.
//
// Thread-safety: every public method is guarded by a mutex so the
// streaming goroutine and the Wails binding can call concurrently.
type Session struct {
	mu sync.Mutex

	protocol   string
	config     map[string]any
	channelMap map[string]string

	tailA          []float64
	tailB          []float64
	samplesEmitted int64 // total sample count processed across all Feed calls
	// Absolute sample-index right after the end of the last event we
	// emitted. We feed the decoder only the samples past this mark so it
	// never sees data from an already-committed frame — that rules out
	// false-positive re-decodes where the signal happens to pass stop-bit
	// validation at a mid-byte boundary.
	commitThrough int64
	haveEmitted   bool
	dtNsLocked    float64
	closed        bool
	// Once the decoder emits an UNDERSAMPLED error (bit period < 2 samples)
	// we silence subsequent ones to avoid flooding the log. Reset when
	// dtNs changes (e.g. switching Fast ↔ Native).
	warnedUndersampled bool
	// Set by ResetTail() to signal that the next Feed starts a new
	// hardware block with no continuity with the previous one. If the
	// decode buffer begins LOW we must skip forward to the first idle
	// HIGH region — otherwise a mid-frame LOW at index 0 would be
	// misread as a start bit and emit a phantom byte per block seam.
	resyncPending bool
}

// Safety margin: keep enough history so any UART / I²C / SPI frame that
// straddles two incoming blocks can still be decoded on the next call.
// 64 k samples = ~82 ms at Fast-streaming dt=1280 ns — enough for a full
// UART frame at 1200 baud (~9 ms) with generous margin, and leaves room
// for higher-baud runs of back-to-back bytes that a smaller tail would
// fragment across blocks.
const sessionTailSamples = 64_000

func NewSession(protocol string, config map[string]any,
	channelMap map[string]string) *Session {
	if config == nil {
		config = map[string]any{}
	}
	if channelMap == nil {
		channelMap = map[string]string{}
	}
	s := &Session{
		protocol:   protocol,
		config:     config,
		channelMap: channelMap,
	}
	log.Printf("[decoder.session] opened protocol=%s config=%+v channelMap=%+v",
		protocol, config, channelMap)
	return s
}

// ResetTail drops the cross-block history without touching the absolute
// sample cursor or the commit mark. Call this between two hardware
// capture blocks so the decoder does not try to stitch a frame across
// a signal gap that does not actually exist in the wire-level data.
// Leaves samplesEmitted / commitThrough intact so event timestamps stay
// monotonic and already-emitted frames are not re-emitted.
func (s *Session) ResetTail() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.tailA = s.tailA[:0]
	s.tailB = s.tailB[:0]
	s.resyncPending = true
}

// Close marks the session as dead. Subsequent Feed calls return empty.
func (s *Session) Close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.closed = true
	s.tailA = nil
	s.tailB = nil
	log.Printf("[decoder.session] closed protocol=%s processed=%d samples",
		s.protocol, s.samplesEmitted)
}

// Feed appends a new block of samples and returns the new, absolute-
// timestamped events (events that already fell inside the previous tail
// window are filtered out).
func (s *Session) Feed(dataA, dataB []float64, dtNs, rangeMvA, rangeMvB float64) []Event {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.closed || len(dataA) == 0 || !(dtNs > 0) {
		return nil
	}
	if dtNs != s.dtNsLocked {
		s.warnedUndersampled = false
	}
	s.dtNsLocked = dtNs

	// Build combined buffers: tail (already-seen samples) + new block.
	oldTailLen := len(s.tailA)
	combinedA := make([]float64, oldTailLen+len(dataA))
	copy(combinedA, s.tailA)
	copy(combinedA[oldTailLen:], dataA)

	var combinedB []float64
	if len(dataB) > 0 || len(s.tailB) > 0 {
		combinedB = make([]float64, len(s.tailB)+len(dataB))
		copy(combinedB, s.tailB)
		copy(combinedB[len(s.tailB):], dataB)
	}

	// baseSample is the absolute sample index of combinedA[0].
	baseSample := s.samplesEmitted - int64(oldTailLen)
	s.samplesEmitted += int64(len(dataA))

	// Trim the leading portion that ends before `commitThrough` — we've
	// already emitted every frame in that region, and forcing the decoder
	// to re-scan it is how spurious stop-bit alignments slipped in.
	trim := 0
	if s.haveEmitted {
		if s.commitThrough > baseSample {
			trim = int(s.commitThrough - baseSample)
		}
	}
	if trim >= len(combinedA) {
		// Nothing new past the last committed frame — just update the tail.
		s.rotateTail(combinedA, combinedB)
		return nil
	}
	decodeA := combinedA[trim:]
	var decodeB []float64
	if combinedB != nil {
		if trim < len(combinedB) {
			decodeB = combinedB[trim:]
		}
	}
	// Sample index of decodeA[0] in absolute coords.
	decodeBase := baseSample + int64(trim)

	// Post-ResetTail resync: if the new block begins while the signal is
	// LOW we are guaranteed to be mid-frame (the previous hardware block
	// cut us off inside a byte). Walk forward until the line has been
	// idle HIGH for ~1 bit period before decoding — otherwise the start
	// of decodeA looks like a spurious start bit and the decoder emits a
	// phantom byte at every block seam.
	if s.resyncPending {
		s.resyncPending = false
		if len(decodeA) > 0 && s.protocol == "uart" {
			baud := 9600.0
			if v, ok := s.config["baud"].(float64); ok && v > 0 {
				baud = v
			}
			bitPeriodSmp := (1e9 / baud) / dtNs
			idleReq := int(bitPeriodSmp + 0.5)
			if idleReq < 1 {
				idleReq = 1
			}
			// Quick amplitude-based logic decision — matches the
			// decoder's own threshold fallback closely enough to find
			// the first HIGH run.
			thr := 0.0
			if len(decodeA) > 0 {
				lo, hi := decodeA[0], decodeA[0]
				for _, v := range decodeA {
					if v < lo {
						lo = v
					}
					if v > hi {
						hi = v
					}
				}
				thr = (lo + hi) / 2
			}
			isHigh := func(v float64) bool { return v > thr }
			if !isHigh(decodeA[0]) {
				skip := 0
				highRun := 0
				for skip < len(decodeA)-1 && highRun < idleReq {
					if isHigh(decodeA[skip]) {
						highRun++
					} else {
						highRun = 0
					}
					skip++
				}
				if skip >= len(decodeA)-1 {
					s.rotateTail(combinedA, combinedB)
					return nil
				}
				decodeA = decodeA[skip:]
				if decodeB != nil && skip < len(decodeB) {
					decodeB = decodeB[skip:]
				} else if decodeB != nil {
					decodeB = nil
				}
				decodeBase += int64(skip)
			}
		}
	}

	ctx := Context{
		SamplesA:   decodeA,
		SamplesB:   decodeB,
		DtNs:       dtNs,
		RangeMvA:   rangeMvA,
		RangeMvB:   rangeMvB,
		Config:     s.config,
		ChannelMap: s.channelMap,
	}

	var events []Event
	var dbg []string
	var err error
	switch s.protocol {
	case "uart":
		events, dbg, err = DecodeUART(ctx)
	case "i2c":
		events, dbg, err = DecodeI2C(ctx)
	case "spi":
		events, dbg, err = DecodeSPI(ctx)
	case "can":
		events, dbg, err = DecodeCAN(ctx)
	case "dcf77":
		events, dbg, err = DecodeDCF77(ctx)
	case "irig", "irigb":
		events, dbg, err = DecodeIRIG(ctx)
	case "afnor":
		events, dbg, err = DecodeAFNOR(ctx)
	case "timecode_auto":
		events, dbg, err = decodeTimecodeAuto(ctx)
	}
	for _, line := range dbg {
		log.Printf("[decoder.%s] %s", s.protocol, line)
	}
	if err != nil {
		log.Printf("[decoder.session] %s error: %v", s.protocol, err)
		s.rotateTail(combinedA, combinedB)
		return nil
	}

	// Re-stamp with absolute timestamps and advance the commit cursor.
	out := make([]Event, 0, len(events))
	for _, e := range events {
		// Dedupe the UNDERSAMPLED warning: emit once, silence until dtNs
		// changes. Without this, native streaming (100 S/s, bit period
		// < 0.02 samples at any reasonable baud) would flood the log.
		if e.Annotation == "UNDERSAMPLED" {
			if s.warnedUndersampled {
				continue
			}
			s.warnedUndersampled = true
		}
		startSample := decodeBase + int64(e.TNs/dtNs+0.5)
		endSample := startSample
		if e.TEndNs > 0 {
			endSample = decodeBase + int64(e.TEndNs/dtNs+0.5)
		}
		e.TNs = float64(startSample) * dtNs
		e.TEndNs = float64(endSample) * dtNs
		out = append(out, e)
		if endSample > s.commitThrough {
			s.commitThrough = endSample
		}
		s.haveEmitted = true
	}

	s.rotateTail(combinedA, combinedB)
	// Log every Feed (not just event-producing ones) so we can correlate the
	// UI's TB / Time-div / range settings with what the decoder actually sees.
	log.Printf("[decoder.session] %s Feed block=%d decode=%d dt_ns=%.1f "+
		"rangeA_mv=%.0f rangeB_mv=%.0f commit=%d events=+%d",
		s.protocol, len(dataA), len(decodeA), dtNs,
		rangeMvA, rangeMvB, s.commitThrough, len(out))
	return out
}

// rotateTail keeps the last sessionTailSamples of each combined buffer so
// the next Feed can decode frames that straddled the boundary.
func (s *Session) rotateTail(combinedA, combinedB []float64) {
	if len(combinedA) > sessionTailSamples {
		s.tailA = append(s.tailA[:0], combinedA[len(combinedA)-sessionTailSamples:]...)
	} else {
		s.tailA = append(s.tailA[:0], combinedA...)
	}
	if combinedB != nil {
		if len(combinedB) > sessionTailSamples {
			s.tailB = append(s.tailB[:0], combinedB[len(combinedB)-sessionTailSamples:]...)
		} else {
			s.tailB = append(s.tailB[:0], combinedB...)
		}
	}
}
