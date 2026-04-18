package decoder

import (
	"fmt"
	"math"
)

// CANConfig carries the few knobs the user can tweak from the UI. The
// decoder always expects a single-wire view (CAN_L referenced to V_diff or a
// differential probe) where recessive is HIGH and dominant is LOW.
type CANConfig struct {
	BitRate       float64
	SamplePoint   float64 // 0..1 fraction into the bit
	ThresholdMv   float64
	AutoThreshold bool
}

func canConfigFrom(m map[string]any) CANConfig {
	c := CANConfig{
		BitRate: 500_000, SamplePoint: 0.625,
		ThresholdMv: 1500, AutoThreshold: true,
	}
	if v, ok := m["bitRate"].(float64); ok && v > 0 {
		c.BitRate = v
	}
	if v, ok := m["samplePoint"].(float64); ok && v > 0 && v < 1 {
		c.SamplePoint = v
	}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	return c
}

// canPickThresholds reuses the UART bimodal logic; for a 3.3 V differential
// receiver we typically see a generous swing so the auto path wins.
func canPickThresholds(samples []float64, cfg CANConfig) (float64, float64, string) {
	fixed := func(tag string) (float64, float64, string) {
		return cfg.ThresholdMv * 1.10, cfg.ThresholdMv * 0.90, tag
	}
	if !cfg.AutoThreshold {
		return fixed("fixed")
	}
	if len(samples) < 32 {
		return fixed("fixed-short")
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

// DecodeCAN parses a single CAN frame (or a sequence of them) from a single
// digitised line. It handles standard (11-bit) and extended (29-bit) IDs,
// bit de-stuffing, CRC-15 verification, RTR, DLC, and end-of-frame.
func DecodeCAN(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event
	cfg := canConfigFrom(ctx.Config)

	chKey := "A"
	if m, ok := ctx.ChannelMap["CAN"]; ok && (m == "A" || m == "B") {
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

	vHigh, vLow, thrMode := canPickThresholds(samples, cfg)
	logic := SchmittTrigger(samples, vHigh, vLow, 1)

	bitSmp := 1e9 / (cfg.BitRate * ctx.DtNs)
	if bitSmp < 2 {
		events = append(events, Event{
			Kind: "error", Level: "error", Annotation: "UNDERSAMPLED",
			Text: fmt.Sprintf("bit period = %.2f samples (<2)", bitSmp),
		})
		return events, dbg, nil
	}

	dbg = append(dbg, fmt.Sprintf("can samples=%d dt_ns=%.1f bitSmp=%.2f threshold=%s vHigh=%.0f vLow=%.0f cfg=%+v",
		len(samples), ctx.DtNs, bitSmp, thrMode, vHigh, vLow, cfg))

	n := len(logic)
	i := 0
	// CAN frames start on a HIGH→LOW edge (Start-of-Frame = dominant). We
	// walk the logic line, lock onto each SOF, read the frame, and skip past
	// the EOF/IFS before looking for the next one.
	for i < n-1 {
		// Scan for a falling edge (recessive → dominant).
		for i < n-1 && !(logic[i] == 1 && logic[i+1] == 0) {
			i++
		}
		if i >= n-1 {
			break
		}
		sofIdx := i + 1

		// Read bits at their sample point, de-stuffing as we go. Stop when
		// the stuff rule finds a 6th identical bit (stuff error) or when
		// we've read 7 recessive bits in a row (EOF).
		readBitAt := func(sampleIdx float64) int {
			return SampleAt(logic, sampleIdx)
		}

		var raw []int     // pre-destuff
		var cooked []int  // post-destuff (what the decoder consumes)
		var cookedT []int // sample index for each cooked bit
		prev := -1
		runLen := 0
		stuffed := false
		bitCount := 0

		maxBits := 200 // cap at ~standard frame + margin
		for k := 0; k < maxBits; k++ {
			centre := float64(sofIdx) + bitSmp*(float64(k)+cfg.SamplePoint)
			b := readBitAt(centre)
			if b < 0 {
				break
			}
			raw = append(raw, b)

			if stuffed {
				// This is a stuffed bit: must be opposite of the previous
				// five identical bits. Reset run and skip.
				if b == prev {
					events = append(events, Event{
						TNs: centre * ctx.DtNs, Kind: "error", Level: "error",
						Annotation: "STUFF",
						Text:       "stuff-bit violation",
					})
				}
				stuffed = false
				prev = b
				runLen = 1
				continue
			}
			cooked = append(cooked, b)
			cookedT = append(cookedT, int(centre+0.5))
			bitCount++

			if b == prev {
				runLen++
				if runLen == 5 {
					// The next bit on the wire is a stuff bit.
					stuffed = true
				}
			} else {
				prev = b
				runLen = 1
			}

			// EOF detection: 7 recessive bits at the end of the frame.
			// The receiver can only declare EOF once enough fields have
			// been consumed, so we defer this check until after field parsing.
		}

		if len(cooked) < 19 {
			// Not enough bits to be a frame; move on after consuming one
			// bit to avoid infinite loops on noisy idle.
			i = sofIdx + 1
			continue
		}

		// Field layout (post-destuff, MSB-first):
		//   [0]       SOF (dominant)
		//   [1..11]   11-bit base ID
		//   [12]      RTR (std) or SRR (ext)
		//   [13]      IDE (0 = std, 1 = ext)
		//   If IDE=1:
		//     [14..31] 18-bit extension ID
		//     [32]     RTR
		//     [33..34] r1, r0
		//     [35..38] DLC
		//     data + CRC follow.
		//   If IDE=0:
		//     [14]     r0
		//     [15..18] DLC
		//     data + CRC follow.
		if cooked[0] != 0 {
			i = sofIdx + 1
			continue
		}
		id := 0
		for k := 1; k <= 11; k++ {
			id = (id << 1) | cooked[k]
		}
		rtrOrSrr := cooked[12]
		ide := cooked[13]

		var dlc int
		var dataStart int
		var extID int
		isExt := ide == 1
		rtr := false
		if isExt {
			if len(cooked) < 40 {
				i = sofIdx + 1
				continue
			}
			for k := 14; k < 32; k++ {
				extID = (extID << 1) | cooked[k]
			}
			rtr = cooked[32] == 1
			// skip r1, r0 at 33..34
			for k := 35; k < 39; k++ {
				dlc = (dlc << 1) | cooked[k]
			}
			dataStart = 39
		} else {
			rtr = rtrOrSrr == 1
			// r0 at 14
			for k := 15; k < 19; k++ {
				dlc = (dlc << 1) | cooked[k]
			}
			dataStart = 19
		}
		if dlc > 8 {
			dlc = 8
		}

		dataBits := 0
		if !rtr {
			dataBits = dlc * 8
		}
		crcStart := dataStart + dataBits
		if crcStart+15 > len(cooked) {
			i = sofIdx + 1
			continue
		}

		var data []byte
		for b := 0; b < dlc && !rtr; b++ {
			v := 0
			off := dataStart + b*8
			for k := 0; k < 8; k++ {
				v = (v << 1) | cooked[off+k]
			}
			data = append(data, byte(v))
		}

		wireCRC := 0
		for k := 0; k < 15; k++ {
			wireCRC = (wireCRC << 1) | cooked[crcStart+k]
		}
		calcCRC := 0
		for _, b := range cooked[:crcStart] {
			nx := ((calcCRC >> 14) ^ b) & 1
			calcCRC = (calcCRC << 1) & 0x7FFF
			if nx == 1 {
				calcCRC ^= 0x4599
			}
		}
		crcOk := wireCRC == calcCRC

		// Frame time window (approximate): SOF through end of CRC.
		tStart := float64(sofIdx) * ctx.DtNs
		tEnd := float64(cookedT[crcStart+14]) * ctx.DtNs

		var fullID int
		if isExt {
			fullID = (id << 18) | extID
		} else {
			fullID = id
		}

		kind := "frame"
		level := "info"
		if !crcOk {
			kind = "frame_crc_err"
			level = "error"
		}
		idTag := fmt.Sprintf("0x%X", fullID)
		if isExt {
			idTag = fmt.Sprintf("0x%08X ext", fullID)
		}
		rtrTag := ""
		if rtr {
			rtrTag = " RTR"
		}
		dataTag := ""
		for _, b := range data {
			dataTag += fmt.Sprintf(" %02X", b)
		}
		events = append(events, Event{
			TNs: tStart, TEndNs: tEnd, Kind: kind, Level: level,
			Value: fullID, HasValue: true,
			Annotation: fmt.Sprintf("ID=%s DLC=%d%s%s", idTag, dlc, rtrTag, dataTag),
			Text: fmt.Sprintf("CAN %s DLC=%d%s data=[%s ] crc=0x%04X (%s)",
				idTag, dlc, rtrTag, dataTag, wireCRC,
				map[bool]string{true: "ok", false: "bad"}[crcOk]),
		})

		// Advance past the CRC delimiter + ACK slot + ACK delimiter + EOF
		// (3 framing bits + 7 EOF). We don't strictly verify them; if
		// they're corrupt the next SOF scan will still find the real start.
		afterFrame := crcStart + 15 + 10
		if afterFrame < len(cookedT) {
			i = cookedT[afterFrame] + 1
		} else {
			i = cookedT[len(cookedT)-1] + 1
		}
		_ = raw
	}

	dbg = append(dbg, fmt.Sprintf("can events=%d", len(events)))
	return events, dbg, nil
}
