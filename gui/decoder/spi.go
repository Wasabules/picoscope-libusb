package decoder

import (
	"fmt"
	"math"
)

// SPIConfig mirrors the JSON config coming from the frontend. Defaults cover
// the most common case (mode 0, MSB-first, 8 bits, idle-HIGH CS).
type SPIConfig struct {
	Bits          int // word length, 4..16
	CPOL          int // 0 or 1
	CPHA          int // 0 or 1
	MSBFirst      bool
	CSActiveLow   bool
	ThresholdMv   float64
	AutoThreshold bool
}

func spiConfigFrom(m map[string]any) SPIConfig {
	c := SPIConfig{
		Bits: 8, CPOL: 0, CPHA: 0, MSBFirst: true, CSActiveLow: true,
		ThresholdMv: 1500, AutoThreshold: true,
	}
	if v, ok := m["bits"].(float64); ok {
		b := int(v)
		if b >= 4 && b <= 16 {
			c.Bits = b
		}
	}
	if v, ok := m["cpol"].(float64); ok {
		if int(v) == 1 {
			c.CPOL = 1
		}
	}
	if v, ok := m["cpha"].(float64); ok {
		if int(v) == 1 {
			c.CPHA = 1
		}
	}
	if v, ok := m["msbFirst"].(bool); ok {
		c.MSBFirst = v
	}
	if v, ok := m["csActiveLow"].(bool); ok {
		c.CSActiveLow = v
	}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	return c
}

// spiPickThresholds combines every active line so the decision level lives on
// the same scale regardless of which probe sees the widest swing. Falls back
// to the fixed midpoint when auto detection can't find a real digital signal.
func spiPickThresholds(active [][]float64, cfg SPIConfig) (float64, float64, string) {
	fixed := func(tag string) (float64, float64, string) {
		return cfg.ThresholdMv * 1.10, cfg.ThresholdMv * 0.90, tag
	}
	if !cfg.AutoThreshold {
		return fixed("fixed")
	}
	var all []float64
	for _, a := range active {
		all = append(all, a...)
	}
	if len(all) < 32 {
		return fixed("fixed-short")
	}
	mn, mx := all[0], all[0]
	for _, v := range all {
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

// DecodeSPI samples the MOSI/MISO lines on the clock edge selected by
// CPOL/CPHA while CS is asserted. Both data lines are optional.
func DecodeSPI(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event
	cfg := spiConfigFrom(ctx.Config)

	// Channel roles → A/B/C/D. We only have two physical channels, so the
	// frontend has to pick which two roles it wires to the scope. The role
	// map keys a line to A or B; missing keys disable that line.
	roles := map[string]string{} // role → "A" | "B"
	for _, role := range []string{"SCLK", "MOSI", "MISO", "CS"} {
		if v, ok := ctx.ChannelMap[role]; ok && (v == "A" || v == "B") {
			roles[role] = v
		}
	}
	clkKey, ok := roles["SCLK"]
	if !ok {
		return events, dbg, fmt.Errorf("SPI: SCLK channel not mapped")
	}

	pick := func(k string) []float64 {
		if k == "B" {
			return ctx.SamplesB
		}
		return ctx.SamplesA
	}
	sclk := pick(clkKey)
	if len(sclk) == 0 {
		return events, append(dbg, "no SCLK samples"), nil
	}
	if !(ctx.DtNs > 0) || math.IsNaN(ctx.DtNs) {
		return events, dbg, fmt.Errorf("invalid dt_ns=%v", ctx.DtNs)
	}

	n := len(sclk)
	var mosi, miso, cs []float64
	if k, ok := roles["MOSI"]; ok {
		mosi = pick(k)
		if len(mosi) < n {
			n = len(mosi)
		}
	}
	if k, ok := roles["MISO"]; ok {
		miso = pick(k)
		if len(miso) < n {
			n = len(miso)
		}
	}
	if k, ok := roles["CS"]; ok {
		cs = pick(k)
		if len(cs) < n {
			n = len(cs)
		}
	}

	active := [][]float64{sclk[:n]}
	if mosi != nil {
		active = append(active, mosi[:n])
	}
	if miso != nil {
		active = append(active, miso[:n])
	}
	if cs != nil {
		active = append(active, cs[:n])
	}
	vHigh, vLow, thrMode := spiPickThresholds(active, cfg)

	clkLogic := SchmittTrigger(sclk[:n], vHigh, vLow, cfg.CPOL)
	var mosiLogic, misoLogic, csLogic []uint8
	if mosi != nil {
		mosiLogic = SchmittTrigger(mosi[:n], vHigh, vLow, 0)
	}
	if miso != nil {
		misoLogic = SchmittTrigger(miso[:n], vHigh, vLow, 0)
	}
	if cs != nil {
		idleCS := 1
		if !cfg.CSActiveLow {
			idleCS = 0
		}
		csLogic = SchmittTrigger(cs[:n], vHigh, vLow, idleCS)
	}

	// Sample-edge selection: in mode 0 and mode 3 (CPOL==CPHA) the sample
	// edge is the first (leading) clock transition out of idle — that's a
	// rising edge for CPOL=0, falling for CPOL=1. In mode 1 and mode 2 the
	// sample edge is the second (trailing) transition — falling for CPOL=0,
	// rising for CPOL=1.
	sampleRising := (cfg.CPOL == 0 && cfg.CPHA == 0) ||
		(cfg.CPOL == 1 && cfg.CPHA == 1)

	csAsserted := func(i int) bool {
		if csLogic == nil {
			return true
		}
		if cfg.CSActiveLow {
			return csLogic[i] == 0
		}
		return csLogic[i] == 1
	}

	dbg = append(dbg, fmt.Sprintf("spi samples=%d dt_ns=%.1f threshold=%s vHigh=%.0f vLow=%.0f cfg=%+v sampleRising=%v",
		n, ctx.DtNs, thrMode, vHigh, vLow, cfg, sampleRising))

	// Walk the clock. On each sampling edge while CS is asserted, shift in
	// one bit from MOSI and MISO. Every Bits samples, emit a word.
	mosiBits := make([]int, 0, cfg.Bits)
	misoBits := make([]int, 0, cfg.Bits)
	var wordStart int

	emitWord := func(endIdx int) {
		if len(mosiBits) == 0 && len(misoBits) == 0 {
			return
		}
		tStart := float64(wordStart) * ctx.DtNs
		tEnd := float64(endIdx) * ctx.DtNs
		// Pad short words to Bits so the bit-packing is consistent.
		for len(mosiBits) < cfg.Bits {
			mosiBits = append(mosiBits, 0)
		}
		for len(misoBits) < cfg.Bits {
			misoBits = append(misoBits, 0)
		}
		var mosiVal, misoVal int
		if cfg.MSBFirst {
			mosiVal = BitsToIntMSB(mosiBits)
			misoVal = BitsToIntMSB(misoBits)
		} else {
			mosiVal = BitsToIntLSB(mosiBits)
			misoVal = BitsToIntLSB(misoBits)
		}
		if mosi != nil {
			events = append(events, Event{
				TNs: tStart, TEndNs: tEnd, Kind: "mosi", Level: "info",
				Value: mosiVal, HasValue: true,
				Annotation: fmt.Sprintf("%02X", mosiVal),
				Text:       fmt.Sprintf("MOSI 0x%02X", mosiVal),
			})
		}
		if miso != nil {
			events = append(events, Event{
				TNs: tStart, TEndNs: tEnd, Kind: "miso", Level: "info",
				Value: misoVal, HasValue: true,
				Annotation: fmt.Sprintf("%02X", misoVal),
				Text:       fmt.Sprintf("MISO 0x%02X", misoVal),
			})
		}
		mosiBits = mosiBits[:0]
		misoBits = misoBits[:0]
	}

	// Optionally emit CS-assert / CS-release markers to bracket bursts.
	prevCS := true
	if csLogic != nil {
		prevCS = csAsserted(0)
		if prevCS {
			events = append(events, Event{
				TNs: 0, Kind: "cs_assert", Level: "info",
				Annotation: "CS↓", Text: "CS asserted",
			})
		}
	}

	for i := 1; i < n; i++ {
		if csLogic != nil {
			cur := csAsserted(i)
			if cur != prevCS {
				tEv := float64(i) * ctx.DtNs
				if cur {
					events = append(events, Event{
						TNs: tEv, Kind: "cs_assert", Level: "info",
						Annotation: "CS↓", Text: "CS asserted",
					})
					mosiBits = mosiBits[:0]
					misoBits = misoBits[:0]
				} else {
					// flush any in-flight word at CS release
					emitWord(i)
					events = append(events, Event{
						TNs: tEv, Kind: "cs_release", Level: "info",
						Annotation: "CS↑", Text: "CS released",
					})
				}
				prevCS = cur
			}
			if !cur {
				continue
			}
		}

		prev, cur := clkLogic[i-1], clkLogic[i]
		if prev == cur {
			continue
		}
		isRising := (prev == 0 && cur == 1)
		isSampleEdge := (sampleRising && isRising) || (!sampleRising && !isRising)
		if !isSampleEdge {
			continue
		}
		if len(mosiBits) == 0 && len(misoBits) == 0 {
			wordStart = i
		}
		if mosiLogic != nil {
			mosiBits = append(mosiBits, int(mosiLogic[i]))
		}
		if misoLogic != nil {
			misoBits = append(misoBits, int(misoLogic[i]))
		}
		// Symmetric append: the bit counters should stay in lockstep even
		// if only one line is connected.
		maxLen := len(mosiBits)
		if len(misoBits) > maxLen {
			maxLen = len(misoBits)
		}
		if maxLen >= cfg.Bits {
			emitWord(i)
		}
	}

	// If CS was still asserted at the end, flush whatever we've accumulated.
	if csLogic == nil || csAsserted(n-1) {
		emitWord(n - 1)
	}

	dbg = append(dbg, fmt.Sprintf("spi events=%d", len(events)))
	return events, dbg, nil
}
