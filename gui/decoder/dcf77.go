package decoder

import (
	"fmt"
	"math"
)

// DCF77Config lets the user override the auto-threshold if the AM envelope
// is particularly noisy (weak antenna).
type DCF77Config struct {
	ThresholdMv   float64
	AutoThreshold bool
}

func dcf77ConfigFrom(m map[string]any) DCF77Config {
	c := DCF77Config{ThresholdMv: 1500, AutoThreshold: true}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	return c
}

// DecodeDCF77 parses the German longwave time signal carried as an AM
// envelope. We work on the digitised envelope (a pulse train at 1 Hz with
// 100 ms LOW = bit 0 and 200 ms LOW = bit 1; second 59 is silent to mark
// the minute boundary).
func DecodeDCF77(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event
	cfg := dcf77ConfigFrom(ctx.Config)

	chKey := "A"
	if m, ok := ctx.ChannelMap["DATA"]; ok && (m == "A" || m == "B") {
		chKey = m
	}
	var samples []float64
	if chKey == "B" {
		samples = ctx.SamplesB
	} else {
		samples = ctx.SamplesA
	}
	if len(samples) == 0 {
		return events, append(dbg, "no samples"), nil
	}
	if !(ctx.DtNs > 0) || math.IsNaN(ctx.DtNs) {
		return events, dbg, fmt.Errorf("invalid dt_ns=%v", ctx.DtNs)
	}

	vHigh, vLow, thrMode := dcf77PickThresholds(samples, cfg)
	logic := SchmittTrigger(samples, vHigh, vLow, 1)

	// Walk once, collecting (start_sample, width_ms) for every LOW pulse.
	type pulse struct {
		start int
		ms    float64
	}
	var pulses []pulse
	i := 0
	for i < len(logic) {
		if logic[i] == 0 {
			s := i
			for i < len(logic) && logic[i] == 0 {
				i++
			}
			pulses = append(pulses, pulse{start: s, ms: float64(i-s) * ctx.DtNs * 1e-6})
		} else {
			i++
		}
	}
	dbg = append(dbg, fmt.Sprintf("dcf77 samples=%d dt_ns=%.1f pulses=%d threshold=%s",
		len(samples), ctx.DtNs, len(pulses), thrMode))
	if len(pulses) < 2 {
		return events, dbg, nil
	}

	// Find the minute mark: the ≥1.8 s gap between consecutive pulses.
	// That gap sits between bit 58 (last pulse of the minute) and bit 0 of
	// the next minute. We anchor decoding at the pulse immediately after
	// any such gap.
	sr := 1e9 / ctx.DtNs
	var frameStarts []int
	for k := 1; k < len(pulses); k++ {
		gapSmp := pulses[k].start - (pulses[k-1].start + int(pulses[k-1].ms*1e-3*sr+0.5))
		gapMs := float64(gapSmp) * ctx.DtNs * 1e-6
		if gapMs > 1500 {
			frameStarts = append(frameStarts, k)
		}
	}
	if len(frameStarts) == 0 {
		events = append(events, Event{
			Kind: "warn", Level: "warn", Annotation: "NO MINUTE MARK",
			Text: "DCF77: no silent second detected — need ≥61 s of capture",
		})
		return events, dbg, nil
	}

	// Decode every complete frame we can find. A frame is 59 consecutive
	// pulses starting at frameStarts[f]; the next silent second must be
	// ~60 seconds later, so frameStarts[f+1] - frameStarts[f] should be 59.
	for f := 0; f < len(frameStarts); f++ {
		start := frameStarts[f]
		end := start + 59
		if end > len(pulses) {
			break
		}
		bits := make([]int, 59)
		for k := 0; k < 59; k++ {
			p := pulses[start+k]
			switch {
			case p.ms < 70 || p.ms > 240:
				bits[k] = -1
			case p.ms < 150:
				bits[k] = 0
			default:
				bits[k] = 1
			}
		}
		minUnits := bitsLSB(bits, 21, 4)
		minTens := bitsLSB(bits, 25, 3)
		hourUnits := bitsLSB(bits, 29, 4)
		hourTens := bitsLSB(bits, 33, 2)
		dayUnits := bitsLSB(bits, 36, 4)
		dayTens := bitsLSB(bits, 40, 2)
		dow := bitsLSB(bits, 42, 3)
		monthUnits := bitsLSB(bits, 45, 4)
		monthTens := bitsLSB(bits, 49, 1)
		yearUnits := bitsLSB(bits, 50, 4)
		yearTens := bitsLSB(bits, 54, 4)
		min := minTens*10 + minUnits
		hour := hourTens*10 + hourUnits
		day := dayTens*10 + dayUnits
		month := monthTens*10 + monthUnits
		year := yearTens*10 + yearUnits

		tStart := float64(pulses[start].start) * ctx.DtNs
		tEnd := tStart + 60e9 // one minute later
		text := fmt.Sprintf("DCF77 20%02d-%02d-%02d %02d:%02d DOW=%d",
			year, month, day, hour, min, dow)
		events = append(events, Event{
			TNs: tStart, TEndNs: tEnd, Kind: "dcf77_frame", Level: "info",
			Annotation: fmt.Sprintf("%02d:%02d", hour, min),
			Text:       text,
		})
	}

	return events, dbg, nil
}

// dcf77PickThresholds is a trimmed copy of the UART picker — DCF77 samples
// are a clean-ish AM envelope so the bimodal test isn't strictly needed.
func dcf77PickThresholds(samples []float64, cfg DCF77Config) (float64, float64, string) {
	fixed := func(tag string) (float64, float64, string) {
		return cfg.ThresholdMv * 1.10, cfg.ThresholdMv * 0.90, tag
	}
	if !cfg.AutoThreshold || len(samples) < 32 {
		return fixed("fixed")
	}
	mn, mx := samples[0], samples[0]
	for _, v := range samples {
		if v < mn {
			mn = v
		}
		if v > mx {
			mx = v
		}
	}
	pp := mx - mn
	if pp < 600 {
		return fixed("fixed-idle")
	}
	mid := (mn + mx) * 0.5
	hyst := pp * 0.2
	return mid + hyst, mid - hyst, "auto"
}

// bitsLSB packs a contiguous range of bits (LSB-first) into an int. Returns
// 0 if any bit in the range is -1 (malformed).
func bitsLSB(bits []int, start, count int) int {
	v := 0
	for k := 0; k < count; k++ {
		if start+k >= len(bits) {
			return 0
		}
		b := bits[start+k]
		if b < 0 {
			return 0
		}
		if b == 1 {
			v |= 1 << k
		}
	}
	return v
}
