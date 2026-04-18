package decoder

// SchmittTrigger digitises an analog waveform using a two-threshold comparator.
// Returns one bit per input sample. `initial` is the state used before the
// first threshold crossing (typical: 1 for idle-HIGH lines, 0 otherwise).
func SchmittTrigger(samples []float64, vHigh, vLow float64, initial int) []uint8 {
	n := len(samples)
	out := make([]uint8, n)
	s := uint8(0)
	if initial != 0 {
		s = 1
	}
	for i := 0; i < n; i++ {
		v := samples[i]
		if s == 0 && v >= vHigh {
			s = 1
		} else if s == 1 && v <= vLow {
			s = 0
		}
		out[i] = s
	}
	return out
}

// SampleAt returns logic[round(sampleIdx)] or -1 if the index is out of range.
func SampleAt(logic []uint8, sampleIdx float64) int {
	i := int(sampleIdx + 0.5)
	if i < 0 || i >= len(logic) {
		return -1
	}
	return int(logic[i])
}

// BitsToIntLSB packs {D0, D1, ...} into an integer (D0 = bit 0).
func BitsToIntLSB(bits []int) int {
	v := 0
	for i, b := range bits {
		if b == 1 {
			v |= 1 << i
		}
	}
	return v
}

// BitsToIntMSB is the MSB-first variant — the caller's first bit becomes the
// high bit.
func BitsToIntMSB(bits []int) int {
	v := 0
	n := len(bits)
	for i, b := range bits {
		if b == 1 {
			v |= 1 << (n - 1 - i)
		}
	}
	return v
}
