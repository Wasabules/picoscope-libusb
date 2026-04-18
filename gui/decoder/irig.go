package decoder

import (
	"fmt"
	"math"
)

// IRIGConfig chooses the IRIG variant. For DC-shift the pulse widths are
// 2 ms / 5 ms / 8 ms regardless of sub-format; the sub-format only changes
// which optional fields (straight binary seconds, control functions) sit
// past the TOY block. The decoder extracts the core time fields always and
// tags the variant for display.
type IRIGConfig struct {
	Variant       string // "B000".."B007" or "auto"
	ThresholdMv   float64
	AutoThreshold bool
}

func irigConfigFrom(m map[string]any) IRIGConfig {
	c := IRIGConfig{Variant: "auto", ThresholdMv: 1500, AutoThreshold: true}
	if v, ok := m["variant"].(string); ok && v != "" {
		c.Variant = v
	}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	return c
}

// DecodeIRIG implements the IRIG-B DC-shift time-code state machine.
// The envelope carries 100 slot/second; each slot begins with a LOW pulse
// of 2 ms (bit 0), 5 ms (bit 1), or 8 ms (position identifier). The frame
// starts at the reference marker "Pr" which is followed by a second 8 ms
// pulse one slot later — two consecutive 8 ms pulses mark the TOY frame
// start. Fields: seconds (1..8), minutes (10..17), hours (20..26),
// day-of-year (30..41), year (50..57). Remaining slots are control
// functions (CF) and straight binary seconds (SBS) which vary per variant.
func DecodeIRIG(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event
	cfg := irigConfigFrom(ctx.Config)

	chKey := "A"
	if m, ok := ctx.ChannelMap["DATA"]; ok && (m == "A" || m == "B") {
		chKey = m
	}
	samples := ctx.SamplesA
	if chKey == "B" {
		samples = ctx.SamplesB
	}
	if len(samples) == 0 {
		return events, append(dbg, "no samples"), nil
	}
	if !(ctx.DtNs > 0) || math.IsNaN(ctx.DtNs) {
		return events, dbg, fmt.Errorf("invalid dt_ns=%v", ctx.DtNs)
	}

	vHigh, vLow, thrMode := dcf77PickThresholds(samples, DCF77Config{
		ThresholdMv: cfg.ThresholdMv, AutoThreshold: cfg.AutoThreshold,
	})
	logic := SchmittTrigger(samples, vHigh, vLow, 1)

	// Collect every LOW pulse (start index, width in ms, symbol).
	type pulse struct {
		start  int
		ms     float64
		symbol int // 0, 1, or 2 (position identifier)
	}
	var pulses []pulse
	for i := 0; i < len(logic); {
		if logic[i] == 0 {
			s := i
			for i < len(logic) && logic[i] == 0 {
				i++
			}
			ms := float64(i-s) * ctx.DtNs * 1e-6
			sym := -1
			switch {
			case ms >= 1.0 && ms <= 3.5:
				sym = 0
			case ms >= 3.5 && ms <= 6.5:
				sym = 1
			case ms >= 6.5 && ms <= 10.0:
				sym = 2
			}
			pulses = append(pulses, pulse{start: s, ms: ms, symbol: sym})
		} else {
			i++
		}
	}

	dbg = append(dbg, fmt.Sprintf("irig samples=%d dt_ns=%.1f pulses=%d threshold=%s",
		len(samples), ctx.DtNs, len(pulses), thrMode))

	// Find the frame reference: two consecutive position-identifier pulses
	// (Pr at slot 99 + P0 at slot 0). We align the 100-slot window on the
	// first of the two (Pr).
	var frameStarts []int
	for k := 0; k+1 < len(pulses); k++ {
		if pulses[k].symbol == 2 && pulses[k+1].symbol == 2 {
			frameStarts = append(frameStarts, k+1) // P0 is slot 0
		}
	}
	if len(frameStarts) == 0 && len(pulses) >= 100 {
		// Fallback: first pulse is P0 (matches our synth).
		if pulses[0].symbol == 2 {
			frameStarts = []int{0}
		}
	}
	if len(frameStarts) == 0 {
		events = append(events, Event{
			Kind: "warn", Level: "warn", Annotation: "NO FRAME",
			Text: "IRIG-B: could not locate frame reference",
		})
		return events, dbg, nil
	}

	for _, fs := range frameStarts {
		if fs+100 > len(pulses) {
			break
		}
		slot := make([]int, 100)
		for k := 0; k < 100; k++ {
			slot[k] = pulses[fs+k].symbol
		}
		bitsFromSlots := func(start, count int) int {
			b := make([]int, 0, count)
			for k := 0; k < count; k++ {
				s := slot[start+k]
				if s == 2 || s < 0 {
					b = append(b, 0)
					continue
				}
				b = append(b, s)
			}
			return BitsToIntLSB(b)
		}
		sec := bitsFromSlots(1, 4) + 10*bitsFromSlots(5, 4)
		min := bitsFromSlots(10, 4) + 10*bitsFromSlots(15, 3)
		hour := bitsFromSlots(20, 4) + 10*bitsFromSlots(25, 2)
		doy := bitsFromSlots(30, 4) + 10*bitsFromSlots(35, 4) + 100*bitsFromSlots(40, 2)
		year := bitsFromSlots(50, 4) + 10*bitsFromSlots(55, 4)

		tStart := float64(pulses[fs].start) * ctx.DtNs
		tEnd := tStart + 1e9
		variant := cfg.Variant
		if variant == "auto" {
			variant = detectIRIGVariant(slot)
		}
		text := fmt.Sprintf("IRIG-%s 20%02d DOY=%03d %02d:%02d:%02d",
			variant, year, doy, hour, min, sec)
		events = append(events, Event{
			TNs: tStart, TEndNs: tEnd, Kind: "irig_frame", Level: "info",
			Annotation: fmt.Sprintf("%02d:%02d:%02d", hour, min, sec),
			Text:       text,
		})
	}
	return events, dbg, nil
}

// detectIRIGVariant picks B000..B007 from the usage of the CF and SBS
// blocks: B00x encodes no SBS if the SBS slots (80..98) are all zero,
// otherwise B004-B007 is active. The low 3 bits of the suffix are a
// bitfield — here we only distinguish "has SBS" vs "no SBS" which covers
// the two most common production setups.
func detectIRIGVariant(slot []int) string {
	hasSBS := false
	for k := 80; k < 99; k++ {
		if slot[k] == 1 {
			hasSBS = true
			break
		}
	}
	if hasSBS {
		return "B004"
	}
	return "B000"
}
