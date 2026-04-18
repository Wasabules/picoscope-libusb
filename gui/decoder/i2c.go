package decoder

import (
	"fmt"
	"math"
)

// I2CConfig mirrors the JSON config coming from the frontend.
type I2CConfig struct {
	AddrBits      int // 7 or 10
	ThresholdMv   float64
	AutoThreshold bool
}

func i2cConfigFrom(m map[string]any) I2CConfig {
	c := I2CConfig{AddrBits: 7, ThresholdMv: 1500, AutoThreshold: true}
	if v, ok := m["addrMode"].(string); ok {
		if v == "10" {
			c.AddrBits = 10
		}
	}
	// also accept numeric addrBits for symmetry with UART/SPI configs
	if v, ok := m["addrBits"].(float64); ok {
		if int(v) == 10 {
			c.AddrBits = 10
		}
	}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	return c
}

// i2cPickThresholds reuses the bimodal auto-threshold logic from UART. Unlike
// UART, I²C is open-drain so the HIGH rail may be weakly pulled — keep the
// hysteresis generous. When auto fails we revert to the fixed midpoint.
func i2cPickThresholds(scl, sda []float64, cfg I2CConfig) (float64, float64, string) {
	fixed := func(tag string) (float64, float64, string) {
		return cfg.ThresholdMv * 1.10, cfg.ThresholdMv * 0.90, tag
	}
	if !cfg.AutoThreshold {
		return fixed("fixed")
	}
	// Combine the two lines: any sample that's clearly a digital rail on
	// either line informs the global threshold. Both signals sit between
	// 0 and VDD, so this is safe.
	all := make([]float64, 0, len(scl)+len(sda))
	all = append(all, scl...)
	all = append(all, sda...)
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

// i2cState enumerates the decoder's FSM positions.
type i2cState int

const (
	i2cIdle i2cState = iota
	i2cAddr     // collecting the 8 address bits
	i2cAddrAck  // sampling the master→slave ACK for the address byte
	i2cData     // collecting 8 data bits
	i2cDataAck  // sampling the ACK for a data byte
	i2cAddr10   // collecting the second byte of a 10-bit address
	i2cAddr10Ack
)

// DecodeI2C implements the I²C protocol state machine.
func DecodeI2C(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event
	cfg := i2cConfigFrom(ctx.Config)

	sclKey := "A"
	sdaKey := "B"
	if m, ok := ctx.ChannelMap["SCL"]; ok && (m == "A" || m == "B") {
		sclKey = m
	}
	if m, ok := ctx.ChannelMap["SDA"]; ok && (m == "A" || m == "B") {
		sdaKey = m
	}
	if sclKey == sdaKey {
		return events, dbg, fmt.Errorf("SCL and SDA must be on different channels (both=%s)", sclKey)
	}

	pick := func(k string) []float64 {
		if k == "B" {
			return ctx.SamplesB
		}
		return ctx.SamplesA
	}
	scl := pick(sclKey)
	sda := pick(sdaKey)
	if len(scl) == 0 || len(sda) == 0 {
		return events, append(dbg, "no samples"), nil
	}
	if !(ctx.DtNs > 0) || math.IsNaN(ctx.DtNs) {
		return events, dbg, fmt.Errorf("invalid dt_ns=%v", ctx.DtNs)
	}

	n := len(scl)
	if len(sda) < n {
		n = len(sda)
	}

	vHigh, vLow, thrMode := i2cPickThresholds(scl[:n], sda[:n], cfg)
	sclLogic := SchmittTrigger(scl[:n], vHigh, vLow, 1)
	sdaLogic := SchmittTrigger(sda[:n], vHigh, vLow, 1)

	dbg = append(dbg, fmt.Sprintf("i2c samples=%d dt_ns=%.1f threshold=%s vHigh=%.0f vLow=%.0f cfg=%+v",
		n, ctx.DtNs, thrMode, vHigh, vLow, cfg))

	state := i2cIdle
	var byteBits [8]int
	var bitIdx int
	var byteStart int // sample index of first bit of the current byte
	var firstAddrByte int = -1
	var isRead bool

	emitStart := func(idx int, repeated bool) {
		tag := "S"
		text := "START"
		if repeated {
			tag = "Sr"
			text = "REPEATED START"
		}
		events = append(events, Event{
			TNs: float64(idx) * ctx.DtNs, Kind: "start", Level: "info",
			Annotation: tag, Text: text,
		})
	}
	emitStop := func(idx int) {
		events = append(events, Event{
			TNs: float64(idx) * ctx.DtNs, Kind: "stop", Level: "info",
			Annotation: "P", Text: "STOP",
		})
	}

	// Track whether we've seen a START (otherwise bit-sampling is meaningless).
	inTransaction := false

	for i := 1; i < n; i++ {
		sPrev, s := sclLogic[i-1], sclLogic[i]
		dPrev, d := sdaLogic[i-1], sdaLogic[i]

		// START / STOP / Rep.START detection — happens whenever SDA edges
		// while SCL is stable HIGH. We require both sides HIGH to avoid
		// mistaking a clocked bit change for a START.
		if s == 1 && sPrev == 1 {
			if dPrev == 1 && d == 0 {
				// SDA falling while SCL HIGH → START or Rep.START
				repeated := inTransaction
				emitStart(i, repeated)
				state = i2cAddr
				bitIdx = 0
				byteStart = i
				firstAddrByte = -1
				inTransaction = true
				continue
			}
			if dPrev == 0 && d == 1 {
				// SDA rising while SCL HIGH → STOP
				emitStop(i)
				state = i2cIdle
				inTransaction = false
				continue
			}
		}

		// Bit sampling: SCL rising edge and we're collecting bits.
		if sPrev == 0 && s == 1 && inTransaction {
			switch state {
			case i2cAddr, i2cData, i2cAddr10:
				if bitIdx == 0 {
					byteStart = i
				}
				byteBits[bitIdx] = int(d)
				bitIdx++
				if bitIdx == 8 {
					// Decode the 8 bits MSB-first.
					value := 0
					for k := 0; k < 8; k++ {
						value = (value << 1) | byteBits[k]
					}
					tStart := float64(byteStart) * ctx.DtNs
					tEnd := float64(i) * ctx.DtNs
					switch state {
					case i2cAddr:
						if cfg.AddrBits == 10 && (value&0xF8) == 0xF0 {
							// 10-bit address: first byte = 0b11110 AAx R/W
							firstAddrByte = value
							isRead = (value & 0x01) == 1
							state = i2cAddr10Ack
						} else {
							addr := value >> 1
							isRead = (value & 0x01) == 1
							rwTag := "W"
							if isRead {
								rwTag = "R"
							}
							events = append(events, Event{
								TNs: tStart, TEndNs: tEnd, Kind: "addr",
								Level: "info", Value: addr, HasValue: true,
								Annotation: fmt.Sprintf("0x%02X %s", addr, rwTag),
								Text:       fmt.Sprintf("ADDR 0x%02X %s", addr, rwTag),
							})
							state = i2cAddrAck
						}
					case i2cAddr10:
						// Second byte of a 10-bit address: combines with the top 2
						// bits of firstAddrByte (bits 2..1 of the 0b11110xx0 header).
						top := (firstAddrByte >> 1) & 0x03
						addr := (top << 8) | value
						rwTag := "W"
						if isRead {
							rwTag = "R"
						}
						events = append(events, Event{
							TNs: tStart, TEndNs: tEnd, Kind: "addr",
							Level: "info", Value: addr, HasValue: true,
							Annotation: fmt.Sprintf("0x%03X %s", addr, rwTag),
							Text:       fmt.Sprintf("ADDR10 0x%03X %s", addr, rwTag),
						})
						state = i2cAddrAck
					case i2cData:
						events = append(events, Event{
							TNs: tStart, TEndNs: tEnd, Kind: "byte",
							Level: "info", Value: value, HasValue: true,
							Annotation: fmt.Sprintf("0x%02X", value),
							Text:       fmt.Sprintf("DATA 0x%02X", value),
						})
						state = i2cDataAck
					}
					bitIdx = 0
				}
			case i2cAddrAck, i2cDataAck, i2cAddr10Ack:
				// ACK bit: SDA LOW = ACK (slave pulled it down), HIGH = NACK.
				tEv := float64(i) * ctx.DtNs
				if d == 0 {
					events = append(events, Event{
						TNs: tEv, Kind: "ack", Level: "info",
						Annotation: "ACK", Text: "ACK",
					})
				} else {
					events = append(events, Event{
						TNs: tEv, Kind: "nack", Level: "warn",
						Annotation: "NACK", Text: "NACK",
					})
				}
				switch state {
				case i2cAddr10Ack:
					// Expect the second address byte next.
					state = i2cAddr10
					bitIdx = 0
				default:
					// Subsequent bytes in the transaction are data.
					state = i2cData
					bitIdx = 0
				}
			}
		}
	}

	dbg = append(dbg, fmt.Sprintf("i2c events=%d final_state=%d", len(events), int(state)))
	return events, dbg, nil
}
