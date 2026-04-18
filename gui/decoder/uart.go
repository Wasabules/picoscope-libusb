package decoder

import (
	"fmt"
	"math"
	"strconv"
	"strings"
)

// UARTConfig is the subset of the JSON config the UART decoder reads.
// All fields have sensible defaults so a partially-populated config is ok.
type UARTConfig struct {
	Baud          float64
	DataBits      int
	Parity        string // "none" | "even" | "odd"
	StopBits      int
	LSBFirst      bool
	ThresholdMv   float64
	AutoThreshold bool
	// Optional frame delimiters. -1 = disabled. When both are >=0 the
	// decoder emits an extra "packet" event for every SOF…EOF sequence —
	// any bytes outside such a window are still reported individually but
	// the packet layer filters out the alternating-bit noise the bare
	// byte stream picks up between real transmissions.
	Sof int
	Eof int
}

// parseByteConfig accepts an int, float, or string (decimal, "0xAA", or
// bare hex "AA") and returns a 0..255 byte value, or -1 when the field is
// empty / missing / invalid.
func parseByteConfig(v any) int {
	switch x := v.(type) {
	case nil:
		return -1
	case float64:
		if x < 0 || x > 255 {
			return -1
		}
		return int(x)
	case int:
		if x < 0 || x > 255 {
			return -1
		}
		return x
	case string:
		s := strings.TrimSpace(x)
		if s == "" {
			return -1
		}
		base := 10
		if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
			s = s[2:]
			base = 16
		} else if _, err := strconv.ParseInt(s, 10, 64); err != nil {
			// not plain decimal → try hex
			base = 16
		}
		n, err := strconv.ParseInt(s, base, 64)
		if err != nil || n < 0 || n > 255 {
			return -1
		}
		return int(n)
	}
	return -1
}

func uartConfigFrom(m map[string]any) UARTConfig {
	c := UARTConfig{
		Baud: 9600, DataBits: 8, Parity: "none", StopBits: 1,
		LSBFirst: true, ThresholdMv: 1500, AutoThreshold: true,
		Sof: -1, Eof: -1,
	}
	if v, ok := m["baud"].(float64); ok && v > 0 {
		c.Baud = v
	}
	if v, ok := m["dataBits"].(float64); ok {
		c.DataBits = int(v)
	}
	if v, ok := m["parity"].(string); ok {
		c.Parity = v
	}
	if v, ok := m["stopBits"].(float64); ok {
		c.StopBits = int(v)
	}
	if v, ok := m["lsbFirst"].(bool); ok {
		c.LSBFirst = v
	}
	if v, ok := m["thresholdMv"].(float64); ok {
		c.ThresholdMv = v
	}
	if v, ok := m["autoThreshold"].(bool); ok {
		c.AutoThreshold = v
	}
	if v, ok := m["sof"]; ok {
		c.Sof = parseByteConfig(v)
	}
	if v, ok := m["eof"]; ok {
		c.Eof = parseByteConfig(v)
	}
	return c
}

// pickSchmittThresholds returns (vHigh, vLow) to feed the Schmitt trigger.
// When autoThreshold is on and the captured window looks like a real binary
// signal (large swing AND most samples sit near the two rails rather than
// in the middle), we place the midpoint at (min+max)/2 with generous
// hysteresis — this handles signals clipped by a narrow scope range
// (e.g. 3.3 V TTL on a 1 V range). In every other case we fall back to the
// fixed level: the failure mode of auto-deriving from a noisy idle line is
// hallucinating thousands of spurious 0x00 bytes, which is worse than
// detecting nothing.
func pickSchmittThresholds(samples []float64, cfg UARTConfig) (float64, float64, string) {
	fixed := func(tag string) (float64, float64, string) {
		return cfg.ThresholdMv * 1.10, cfg.ThresholdMv * 0.90, tag
	}
	if !cfg.AutoThreshold {
		return fixed("fixed")
	}
	n := len(samples)
	if n < 16 {
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
	// Under ~600 mV swing we're almost certainly looking at idle + noise.
	// Auto there gives the Schmitt a near-zero hysteresis band and the
	// decoder hallucinates 0x00 frames on every noise blip.
	if pp < 600 {
		return fixed("fixed-idle")
	}
	// Bimodality: a proper digital line spends almost all its time near one
	// of the two rails, so a histogram has two tall end-bins and a tiny
	// middle. Count how many samples sit in the central 40 % of the swing —
	// if that's more than a quarter of the window the "signal" is dominated
	// by analog transients / noise, not digital levels.
	mid := (mn + mx) * 0.5
	band := pp * 0.2 // ±20 % of the swing around the midpoint = 40 % total
	midHits := 0
	for _, v := range samples {
		if v >= mid-band && v <= mid+band {
			midHits++
		}
	}
	if midHits*4 > n {
		return fixed("fixed-analog")
	}
	hyst := pp * 0.2
	return mid + hyst, mid - hyst, "auto"
}

// DecodeUART implements standard asynchronous serial framing.
func DecodeUART(ctx Context) ([]Event, []string, error) {
	var dbg []string
	var events []Event

	cfg := uartConfigFrom(ctx.Config)

	chKey := "A"
	if m, ok := ctx.ChannelMap["DATA"]; ok && m == "B" {
		chKey = "B"
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

	vHigh, vLow, thrMode := pickSchmittThresholds(samples, cfg)
	logic := SchmittTrigger(samples, vHigh, vLow, 1)

	// Signal stats + edge count — decisive when diagnosing "no bytes decoded":
	// pp too small → fixed-idle path; zero edges → no toggling in the slice.
	mn, mx := samples[0], samples[0]
	for _, v := range samples {
		if v < mn {
			mn = v
		}
		if v > mx {
			mx = v
		}
	}
	edges := 0
	// Collect the length in samples of every contiguous run (HIGH or LOW)
	// so we can see if the bit timing matches bitPeriodSmp. Decisive when
	// the decoder keeps outputting 0xFE/0xFF on a signal that is clearly
	// carrying data: it means the Schmitt smears short bits together and
	// the runs become multiples of 2, 3, … bit periods.
	var runs []int
	if len(logic) > 0 {
		cur := logic[0]
		run := 1
		for k := 1; k < len(logic); k++ {
			if logic[k] != logic[k-1] {
				edges++
				runs = append(runs, run)
				run = 0
				cur = logic[k]
			}
			run++
		}
		_ = cur
		runs = append(runs, run)
	}

	bitPeriodNs := 1e9 / cfg.Baud
	bitPeriodSmp := bitPeriodNs / ctx.DtNs
	if bitPeriodSmp < 2 {
		events = append(events, Event{
			TNs:        0,
			Kind:       "error",
			Level:      "error",
			Annotation: "UNDERSAMPLED",
			Text: fmt.Sprintf("bit period = %.2f samples (<2)",
				bitPeriodSmp),
		})
		return events, append(dbg,
			fmt.Sprintf("undersampled: bitPeriodSmp=%.3f", bitPeriodSmp)), nil
	}

	stopOffset := cfg.DataBits
	if cfg.Parity != "none" {
		stopOffset++
	}
	frameLenSmp := bitPeriodSmp * float64(1+stopOffset+cfg.StopBits)

	dbg = append(dbg, fmt.Sprintf(
		"samples=%d dt_ns=%.1f bitPeriodSmp=%.2f frameLenSmp=%.1f "+
			"sig_min=%.0f sig_max=%.0f sig_pp=%.0f edges=%d "+
			"schmitt=%s vHigh=%.0f vLow=%.0f cfg=%+v",
		len(samples), ctx.DtNs, bitPeriodSmp, frameLenSmp,
		mn, mx, mx-mn, edges,
		thrMode, vHigh, vLow, cfg))
	if edges > 0 && edges < 40 {
		dbg = append(dbg, fmt.Sprintf("runs(samples)=%v", runs))
	}

	// Try to decode a frame starting at `startSample`. Returns the event and
	// the sample index just past the stop bit, or (_, 0, false) on rejection.
	tryFrame := func(startSample int) (Event, int, bool) {
		// mid-start must read 0
		if SampleAt(logic, float64(startSample)+bitPeriodSmp*0.5) != 0 {
			return Event{}, 0, false
		}
		bits := make([]int, cfg.DataBits)
		for b := 0; b < cfg.DataBits; b++ {
			v := SampleAt(logic, float64(startSample)+bitPeriodSmp*(1.5+float64(b)))
			if v < 0 {
				return Event{}, 0, false
			}
			bits[b] = v
		}

		parityOk := true
		if cfg.Parity != "none" {
			pBit := SampleAt(logic, float64(startSample)+
				bitPeriodSmp*(1.5+float64(cfg.DataBits)))
			if pBit < 0 {
				return Event{}, 0, false
			}
			ones := 0
			for _, b := range bits {
				if b == 1 {
					ones++
				}
			}
			expected := ones & 1
			if cfg.Parity == "odd" {
				expected = 1 - expected
			}
			parityOk = (pBit == expected)
		}

		for s := 0; s < cfg.StopBits; s++ {
			sb := SampleAt(logic, float64(startSample)+
				bitPeriodSmp*(1.5+float64(stopOffset)+float64(s)))
			if sb != 1 {
				return Event{}, 0, false
			}
		}

		var value int
		if cfg.LSBFirst {
			value = BitsToIntLSB(bits)
		} else {
			value = BitsToIntMSB(bits)
		}

		endSample := int(float64(startSample) + frameLenSmp + 0.5)
		tStart := float64(startSample) * ctx.DtNs
		tEnd := (float64(startSample) + frameLenSmp) * ctx.DtNs

		ev := Event{
			TNs: tStart, TEndNs: tEnd,
			Value: value, HasValue: true,
		}
		hex := fmt.Sprintf("%02X", value)
		ev.Annotation = hex
		if !parityOk {
			ev.Kind = "error"
			ev.Level = "error"
			ev.Annotation = "PARITY"
			ev.Text = fmt.Sprintf("parity mismatch on 0x%s", hex)
		} else {
			ev.Kind = "byte"
			ev.Level = "info"
			if value >= 0x20 && value < 0x7f {
				ev.Text = fmt.Sprintf("'%c' 0x%s", rune(value), hex)
			} else {
				ev.Text = fmt.Sprintf("0x%s", hex)
			}
		}
		return ev, endSample, true
	}

	n := len(logic)
	i := 0
	safety := n*2 + 16
	var tryCount, acceptCount int
	for i < n-1 && safety > 0 {
		safety--
		// Walk to next HIGH → LOW transition. logic[i] == 0 at entry
		// only happens right after a successful frame (we land on the
		// very next start bit); in that case i already points at it.
		var startSample int
		if logic[i] == 0 {
			startSample = i
		} else {
			for i < n-1 && !(logic[i] == 1 && logic[i+1] == 0) {
				i++
			}
			if i >= n-1 {
				break
			}
			startSample = i + 1
		}
		tryCount++
		ev, endSample, ok := tryFrame(startSample)
		if ok {
			events = append(events, ev)
			acceptCount++
			i = endSample
		} else {
			i = startSample + 1
		}
	}
	dbg = append(dbg, fmt.Sprintf("tries=%d accepted=%d events=%d safety_left=%d",
		tryCount, acceptCount, len(events), safety))

	if cfg.Sof >= 0 && cfg.Eof >= 0 {
		packets := assemblePackets(events, cfg.Sof, cfg.Eof)
		dbg = append(dbg, fmt.Sprintf("packets=%d sof=0x%02X eof=0x%02X",
			len(packets), cfg.Sof, cfg.Eof))
		events = append(events, packets...)
	}

	return events, dbg, nil
}

// assemblePackets walks the byte stream and emits a "packet" event for
// every SOF…EOF window. Byte events outside any complete window are left
// alone (the caller may render them differently). The event's Annotation
// is the hex dump of the payload (excluding the delimiters) and Text is
// the printable-ASCII rendition plus length.
func assemblePackets(events []Event, sof, eof int) []Event {
	var out []Event
	inFrame := false
	var buf []Event
	var startNs float64
	for _, e := range events {
		if e.Kind != "byte" || !e.HasValue {
			continue
		}
		if !inFrame {
			if e.Value == sof {
				inFrame = true
				buf = buf[:0]
				startNs = e.TNs
			}
			continue
		}
		// Nested SOF without an EOF — drop the old window, start fresh.
		if e.Value == sof {
			buf = buf[:0]
			startNs = e.TNs
			continue
		}
		if e.Value == eof {
			out = append(out, buildPacket(buf, startNs, e.TEndNs))
			inFrame = false
			buf = buf[:0]
			continue
		}
		buf = append(buf, e)
	}
	return out
}

func buildPacket(buf []Event, tStart, tEnd float64) Event {
	var hex strings.Builder
	var text strings.Builder
	for i, b := range buf {
		if i > 0 {
			hex.WriteByte(' ')
		}
		fmt.Fprintf(&hex, "%02X", b.Value)
		if b.Value >= 0x20 && b.Value < 0x7f {
			text.WriteByte(byte(b.Value))
		} else {
			text.WriteByte('.')
		}
	}
	return Event{
		TNs: tStart, TEndNs: tEnd,
		Kind: "packet", Level: "info",
		Annotation: hex.String(),
		Text:       fmt.Sprintf("[%d] %q %s", len(buf), text.String(), hex.String()),
	}
}
