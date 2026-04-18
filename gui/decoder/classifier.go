package decoder

import (
	"fmt"
	"math"
	"sort"
)

// TimecodeFormat is the set of protocols the classifier can identify.
type TimecodeFormat string

const (
	TCUnknown TimecodeFormat = "unknown"
	TCDCF77   TimecodeFormat = "dcf77"
	TCIRIGB   TimecodeFormat = "irigb"
	TCAFNOR   TimecodeFormat = "afnor"
)

// TimecodeFingerprint is the classifier output. The UI auto-routes a
// captured signal to the matching decoder when `Confidence` is above a
// threshold; below that the user has to pick manually.
type TimecodeFingerprint struct {
	Format     TimecodeFormat `json:"format"`
	Confidence float64        `json:"confidence"` // 0..1
	PulsesPerS float64        `json:"pulses_per_s"`
	MedianMs   float64        `json:"median_ms"`
	Debug      string         `json:"debug"`
}

// ClassifyTimecode digitises the signal, measures LOW-pulse widths, and
// looks for a signature matching one of the supported time-code families.
// A DCF77 trace has pulses at ~1 Hz with widths near 100 or 200 ms; an
// IRIG-B DC-shift has ~100 pulses/s at widths near 2, 5, or 8 ms; AFNOR
// NF S87-500 has the same carrier timing as IRIG-B but its position
// identifiers arrive in a different sequence — in most cases the pulse-
// width histogram alone cannot tell them apart, so we prefer IRIG-B and
// let the user confirm AFNOR explicitly.
func ClassifyTimecode(samples []float64, dtNs float64) TimecodeFingerprint {
	fp := TimecodeFingerprint{Format: TCUnknown}
	if len(samples) < 1000 || !(dtNs > 0) {
		fp.Debug = "not enough samples"
		return fp
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
		fp.Debug = fmt.Sprintf("signal too flat (pp=%.0f)", pp)
		return fp
	}
	mid := (mn + mx) * 0.5
	hyst := pp * 0.2
	logic := SchmittTrigger(samples, mid+hyst, mid-hyst, 1)

	// Collect LOW-pulse widths in ms.
	var widths []float64
	i := 0
	for i < len(logic) {
		if logic[i] == 0 {
			start := i
			for i < len(logic) && logic[i] == 0 {
				i++
			}
			widths = append(widths, float64(i-start)*dtNs*1e-6)
		} else {
			i++
		}
	}
	if len(widths) == 0 {
		fp.Debug = "no LOW pulses"
		return fp
	}
	totalMs := float64(len(samples)) * dtNs * 1e-6
	fp.PulsesPerS = float64(len(widths)) / (totalMs * 1e-3)

	sorted := append([]float64(nil), widths...)
	sort.Float64s(sorted)
	fp.MedianMs = sorted[len(sorted)/2]

	countNear := func(target, tolerance float64) int {
		c := 0
		for _, w := range widths {
			if math.Abs(w-target) < tolerance {
				c++
			}
		}
		return c
	}

	// DCF77 signature: ~1 pulse/sec, widths near 100 ms or 200 ms.
	if fp.PulsesPerS >= 0.5 && fp.PulsesPerS <= 1.5 {
		n100 := countNear(100, 30)
		n200 := countNear(200, 30)
		match := float64(n100+n200) / float64(len(widths))
		if match > 0.6 {
			fp.Format = TCDCF77
			fp.Confidence = match
			fp.Debug = fmt.Sprintf("dcf77: n100=%d n200=%d total=%d pulses/s=%.2f",
				n100, n200, len(widths), fp.PulsesPerS)
			return fp
		}
	}

	// IRIG-B signature: ~100 pulses/sec, widths near 2 ms / 5 ms / 8 ms.
	if fp.PulsesPerS >= 60 && fp.PulsesPerS <= 130 {
		n2 := countNear(2, 1)
		n5 := countNear(5, 1)
		n8 := countNear(8, 1.5)
		match := float64(n2+n5+n8) / float64(len(widths))
		if match > 0.7 {
			fp.Format = TCIRIGB
			fp.Confidence = match
			fp.Debug = fmt.Sprintf("irigb: n2=%d n5=%d n8=%d total=%d pulses/s=%.2f",
				n2, n5, n8, len(widths), fp.PulsesPerS)
			return fp
		}
	}

	fp.Debug = fmt.Sprintf("no match: %d pulses, %.2f pps, median=%.1fms",
		len(widths), fp.PulsesPerS, fp.MedianMs)
	return fp
}
