package decoder

import (
	"math"
	"math/rand"
)

// SynthUART generates a UART waveform in mV from the bytes to transmit.
// 8-N-1 LSB-first, idle HIGH. Returns samples and the per-sample interval.
//
//   data       bytes to transmit
//   baud       bits per second
//   sampleRate samples per second (e.g. 3.125e6 for TB=5 on the PS2204A)
//   vHigh,vLow logic levels in mV (e.g. 3300, 0 for TTL 3V3)
//   pre,post   number of idle samples before the first start bit and after
//              the last stop bit (helps test realistic "capture starts before
//              traffic" conditions)
func SynthUART(data []byte, baud, sampleRate, vHigh, vLow float64,
	pre, post int) ([]float64, float64) {

	dtNs := 1e9 / sampleRate
	bitSmp := sampleRate / baud
	frameSmp := bitSmp * 10 // 1 start + 8 data + 1 stop
	total := pre + int(math.Ceil(frameSmp*float64(len(data)))) + post
	out := make([]float64, total)
	for i := range out {
		out[i] = vHigh
	}

	for bi, b := range data {
		base := float64(pre) + float64(bi)*frameSmp
		// Bit slots: start(=0), D0..D7, stop(=1)
		levels := make([]float64, 10)
		levels[0] = vLow // start
		for k := 0; k < 8; k++ {
			if (b>>k)&1 == 1 {
				levels[1+k] = vHigh
			} else {
				levels[1+k] = vLow
			}
		}
		levels[9] = vHigh // stop
		for k := 0; k < 10; k++ {
			s := int(base + float64(k)*bitSmp + 0.5)
			e := int(base + float64(k+1)*bitSmp + 0.5)
			if s < 0 {
				s = 0
			}
			if e > total {
				e = total
			}
			for i := s; i < e; i++ {
				out[i] = levels[k]
			}
		}
	}
	return out, dtNs
}

// I2CTransaction describes one complete START…STOP sequence. `Addr` is the
// 7-bit slave address (0..127) or 10-bit (0..1023); `Bytes` is the payload
// (data phase, after addr+R/W). `Ack` is parallel to [addr_ack, byte0_ack, …]
// — one entry per byte sent by the master (slave ack) or received by master
// (master ack). Length = 1 + len(Bytes). Pass true for ACK, false for NACK.
type I2CTransaction struct {
	Addr   int
	Read   bool // R/W bit
	Bytes  []byte
	Ack    []bool
	TenBit bool // 10-bit addressing
}

// SynthI2C generates an open-drain I²C waveform as two mV arrays (SCL, SDA).
// Timing is idealised (no rise-time), but frames are sample-accurate for the
// given clock rate. One START, any number of bytes, then STOP.
//
//   txns       ordered transactions to emit
//   fClk       I²C clock frequency (Hz), e.g. 100_000 or 400_000
//   sampleRate scope sampling rate (Hz)
//   vHigh,vLow logic levels in mV
//   pre, post  idle samples before first START / after last STOP
func SynthI2C(txns []I2CTransaction, fClk, sampleRate, vHigh, vLow float64,
	pre, post int) (scl, sda []float64, dtNs float64) {

	dtNs = 1e9 / sampleRate
	bitSmp := sampleRate / fClk
	// Rough total: per byte we have 9 clock cycles (8 bits + ack). For a
	// 10-bit addr the first address phase eats two bytes = 18 cycles.
	// START/STOP take ~0.5 clock each. Allocate generously, trim at end.
	approx := pre + post
	for _, t := range txns {
		n := 1 + len(t.Bytes)
		if t.TenBit {
			n++
		}
		approx += int(float64(n)*9*bitSmp + 2*bitSmp + 0.5)
	}
	total := approx + int(bitSmp*4)
	scl = make([]float64, total)
	sda = make([]float64, total)
	for i := range scl {
		scl[i] = vHigh
		sda[i] = vHigh
	}

	// Mutable "current level" for each line — we walk a cursor forward and
	// stamp the level into the arrays, updating only when the caller flips
	// the state explicitly.
	curScl := vHigh
	curSda := vHigh
	pos := float64(pre)

	// advance writes the current levels for `dur` samples and moves pos.
	advance := func(dur float64) {
		s := int(pos + 0.5)
		e := int(pos + dur + 0.5)
		if e > total {
			e = total
		}
		if s < 0 {
			s = 0
		}
		for i := s; i < e; i++ {
			scl[i] = curScl
			sda[i] = curSda
		}
		pos = float64(e)
	}

	setScl := func(level float64) { curScl = level }
	setSda := func(level float64) { curSda = level }

	// One bit: SDA setup (SCL LOW), then SCL rises, stays HIGH for a while,
	// then falls. The rising edge is the sampling edge. SDA transitions only
	// occur while SCL is LOW.
	writeBit := func(bit int) {
		if bit == 1 {
			setSda(vHigh)
		} else {
			setSda(vLow)
		}
		setScl(vLow)
		advance(bitSmp * 0.25) // setup: SDA stable, SCL LOW
		setScl(vHigh)
		advance(bitSmp * 0.50) // sample: SCL HIGH, SDA stable
		setScl(vLow)
		advance(bitSmp * 0.25) // hold-back to LOW before next bit
	}

	writeByte := func(b int, ack bool) {
		for k := 7; k >= 0; k-- {
			writeBit((b >> k) & 1)
		}
		ackBit := 1
		if ack {
			ackBit = 0
		}
		writeBit(ackBit)
	}

	// START: both HIGH → SDA falls while SCL HIGH → then SCL falls (so the
	// first bit's setup phase starts from SCL=LOW, matching writeBit).
	writeStart := func() {
		setScl(vHigh)
		setSda(vHigh)
		advance(bitSmp * 0.40) // both idle HIGH a moment
		setSda(vLow)
		advance(bitSmp * 0.30) // SDA LOW, SCL still HIGH — the START edge
		setScl(vLow)
		advance(bitSmp * 0.30) // SCL falls — now ready for writeBit
	}
	// STOP: ensure SCL LOW + SDA LOW, then SCL rises, then SDA rises
	// (stop edge).
	writeStop := func() {
		setScl(vLow)
		setSda(vLow)
		advance(bitSmp * 0.30) // both LOW
		setScl(vHigh)
		advance(bitSmp * 0.30) // SCL HIGH, SDA still LOW
		setSda(vHigh)
		advance(bitSmp * 0.40) // SDA rises — STOP edge
	}

	for _, t := range txns {
		writeStart()
		ackIdx := 0
		if t.TenBit {
			// First byte: 0b11110<a9><a8><R/W>
			first := 0xF0 | ((t.Addr >> 7) & 0x06)
			if t.Read {
				first |= 1
			}
			ackBit := true
			if ackIdx < len(t.Ack) {
				ackBit = t.Ack[ackIdx]
			}
			writeByte(first, ackBit)
			ackIdx++
			// Second byte: lower 8 bits
			writeByte(t.Addr&0xFF, true)
		} else {
			addrByte := (t.Addr & 0x7F) << 1
			if t.Read {
				addrByte |= 1
			}
			ackBit := true
			if ackIdx < len(t.Ack) {
				ackBit = t.Ack[ackIdx]
			}
			writeByte(addrByte, ackBit)
			ackIdx++
		}
		for i, b := range t.Bytes {
			ackBit := true
			if ackIdx+i < len(t.Ack) {
				ackBit = t.Ack[ackIdx+i]
			}
			writeByte(int(b), ackBit)
		}
		writeStop()
	}

	// Pad the rest of the buffers with idle HIGH and trim to the final length.
	end := int(pos+0.5) + post
	if end > len(scl) {
		end = len(scl)
	}
	for i := int(pos + 0.5); i < end; i++ {
		scl[i] = vHigh
		sda[i] = vHigh
	}
	return scl[:end], sda[:end], dtNs
}

// SynthSPI generates SCLK, MOSI, MISO, CS as mV arrays for one transfer.
// Clock `fClk` in Hz. `cpol` is the idle level of SCLK (0 = LOW idle, 1 = HIGH
// idle). `cpha` determines which clock edge samples MOSI:
//
//	cpol=0, cpha=0: sample on SCLK rising
//	cpol=0, cpha=1: sample on SCLK falling
//	cpol=1, cpha=0: sample on SCLK falling
//	cpol=1, cpha=1: sample on SCLK rising
//
// `msbFirst` shifts MSB out first when true (standard Motorola SPI).
// `mosi`, `miso` are the per-byte payloads; pass nil on either side if
// unused. CS is asserted LOW during the transfer and HIGH otherwise.
func SynthSPI(mosi, miso []byte, fClk, sampleRate, vHigh, vLow float64,
	cpol, cpha int, msbFirst bool, pre, post int) (sclk, mo, mi, cs []float64, dtNs float64) {

	dtNs = 1e9 / sampleRate
	bitSmp := sampleRate / fClk
	n := len(mosi)
	if len(miso) > n {
		n = len(miso)
	}
	total := pre + int(float64(n*8+2)*bitSmp+0.5) + post
	sclk = make([]float64, total)
	mo = make([]float64, total)
	mi = make([]float64, total)
	cs = make([]float64, total)

	idleSclk := vLow
	if cpol == 1 {
		idleSclk = vHigh
	}
	for i := range sclk {
		sclk[i] = idleSclk
		mo[i] = vLow
		mi[i] = vLow
		cs[i] = vHigh
	}

	fill := func(arr []float64, from, to int, level float64) {
		if from < 0 {
			from = 0
		}
		if to > len(arr) {
			to = len(arr)
		}
		for i := from; i < to; i++ {
			arr[i] = level
		}
	}

	// CS low from just before the first bit until just after the last.
	pos := float64(pre)
	csStart := int(pos + 0.5)
	csEnd := int(pos + float64(n*8)*bitSmp + bitSmp + 0.5)
	fill(cs, csStart, csEnd, vLow)

	// Each bit slot covers `bitSmp` samples. In cpha=0, data is set before
	// the first edge and sampled on the first edge of the slot. In cpha=1,
	// data is set on the first edge and sampled on the second edge.
	leadEdgeFrac := 0.25
	trailEdgeFrac := 0.75

	writeBit := func(byteIdx, bitInByte int) {
		var mosiBit, misoBit int
		if byteIdx < len(mosi) {
			shift := bitInByte
			if msbFirst {
				shift = 7 - bitInByte
			}
			mosiBit = int((mosi[byteIdx] >> uint(shift)) & 1)
		}
		if byteIdx < len(miso) {
			shift := bitInByte
			if msbFirst {
				shift = 7 - bitInByte
			}
			misoBit = int((miso[byteIdx] >> uint(shift)) & 1)
		}
		s := int(pos + 0.5)
		// Set data levels for the whole slot
		lvMo := vLow
		if mosiBit == 1 {
			lvMo = vHigh
		}
		lvMi := vLow
		if misoBit == 1 {
			lvMi = vHigh
		}
		fill(mo, s, s+int(bitSmp+0.5), lvMo)
		fill(mi, s, s+int(bitSmp+0.5), lvMi)

		// Clock toggles: two edges per slot. Positions of the two edges
		// relative to slot start depend on cpol×cpha, but for simplicity we
		// emit an idle→active→idle pulse centred in the slot. The `cpha`
		// bit flips which edge is the "sampling" one — the decoder must
		// handle both, so we just always emit the canonical symmetric
		// pulse and rely on the decoder picking the correct edge.
		_ = cpha
		lead := s + int(bitSmp*leadEdgeFrac+0.5)
		trail := s + int(bitSmp*trailEdgeFrac+0.5)
		active := vHigh
		if cpol == 1 {
			active = vLow
		}
		fill(sclk, s, lead, idleSclk)
		fill(sclk, lead, trail, active)
		fill(sclk, trail, s+int(bitSmp+0.5), idleSclk)
		pos += bitSmp
	}

	// Let CS settle for half a bit before the first clock.
	pos += bitSmp * 0.5
	for bi := 0; bi < n; bi++ {
		for k := 0; k < 8; k++ {
			writeBit(bi, k)
		}
	}
	return sclk, mo, mi, cs, dtNs
}

// SynthCAN generates a CAN-bus waveform (single-wire view, recessive=HIGH,
// dominant=LOW — matching CAN_L referenced to V_diff). One standard-ID frame.
// Bit-stuffing (5 same → insert opposite) and CRC-15 are applied automatically.
//
//	id          11-bit identifier
//	data        up to 8 bytes
//	bitRateBps  bit rate in bits/s (e.g. 500_000)
type CANFrame struct {
	ID       int
	Extended bool // 29-bit ID
	RTR      bool
	Data     []byte
}

// SynthCAN generates one (or more) CAN frames as a single-signal mV array.
// Recessive (idle) = vHigh, dominant = vLow.
func SynthCAN(frames []CANFrame, bitRateBps, sampleRate, vHigh, vLow float64,
	pre, post int) ([]float64, float64) {

	dtNs := 1e9 / sampleRate
	bitSmp := sampleRate / bitRateBps

	// Build the raw bit stream per frame.
	emitFrame := func(f CANFrame) []int {
		var bits []int
		appendBits := func(v, n int) {
			for k := n - 1; k >= 0; k-- {
				bits = append(bits, (v>>k)&1)
			}
		}
		// SOF dominant
		bits = append(bits, 0)
		if f.Extended {
			// 11-bit base ID
			appendBits((f.ID>>18)&0x7FF, 11)
			// SRR (recessive), IDE (recessive)
			bits = append(bits, 1, 1)
			// 18-bit extension ID
			appendBits(f.ID&0x3FFFF, 18)
			// RTR
			rtr := 0
			if f.RTR {
				rtr = 1
			}
			bits = append(bits, rtr)
			// r1, r0 (reserved dominant)
			bits = append(bits, 0, 0)
		} else {
			// 11-bit ID
			appendBits(f.ID&0x7FF, 11)
			// RTR
			rtr := 0
			if f.RTR {
				rtr = 1
			}
			bits = append(bits, rtr)
			// IDE=0 (standard), r0=0
			bits = append(bits, 0, 0)
		}
		// DLC (4 bits)
		dlc := len(f.Data)
		if f.RTR {
			dlc = 0
		}
		if dlc > 8 {
			dlc = 8
		}
		appendBits(dlc, 4)
		// Data field
		if !f.RTR {
			for i := 0; i < dlc; i++ {
				appendBits(int(f.Data[i]), 8)
			}
		}
		// CRC-15 over SOF through end of data field
		crc := 0
		for _, b := range bits {
			crcNext := ((crc >> 14) ^ b) & 1
			crc = (crc << 1) & 0x7FFF
			if crcNext == 1 {
				crc ^= 0x4599
			}
		}
		crcBits := make([]int, 15)
		for k := 14; k >= 0; k-- {
			crcBits[14-k] = (crc >> k) & 1
		}

		// Bit-stuff the payload (SOF through CRC). After 5 identical bits
		// consecutively, insert an opposite-polarity bit.
		stuffed := []int{}
		prev := -1
		run := 0
		stuffInput := append(append([]int{}, bits...), crcBits...)
		for _, b := range stuffInput {
			stuffed = append(stuffed, b)
			if b == prev {
				run++
				if run == 5 {
					stuffed = append(stuffed, 1-b)
					prev = 1 - b
					run = 1
				}
			} else {
				prev = b
				run = 1
			}
		}
		// Non-stuffed trailer: CRC delimiter (recessive), ACK slot
		// (dominant — receiver drives it), ACK delimiter (recessive),
		// 7 EOF recessive bits, 3 IFS recessive bits.
		stuffed = append(stuffed, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1)
		return stuffed
	}

	var allBits []int
	for _, f := range frames {
		allBits = append(allBits, emitFrame(f)...)
		// Separation between frames
		for i := 0; i < 5; i++ {
			allBits = append(allBits, 1)
		}
	}

	total := pre + int(float64(len(allBits))*bitSmp+0.5) + post
	out := make([]float64, total)
	for i := range out {
		out[i] = vHigh
	}
	pos := float64(pre)
	for _, b := range allBits {
		s := int(pos + 0.5)
		e := int(pos + bitSmp + 0.5)
		if e > total {
			e = total
		}
		lv := vHigh
		if b == 0 {
			lv = vLow
		}
		for i := s; i < e; i++ {
			out[i] = lv
		}
		pos += bitSmp
	}
	return out, dtNs
}

// DCF77Frame holds the 59 data bits that a full minute carries. Bit 0 maps
// to second 0 (start-of-minute mark — LOW/idle), bit 20 is the start-of-
// time-info marker (always 1), and second 59 is silent (used as the
// minute mark). Unused bits in this test scaffolding default to 0.
type DCF77Frame struct {
	MinuteUnits, MinuteTens int
	HourUnits, HourTens     int
	DayUnits, DayTens       int
	DOWMon1Sun7             int
	MonthUnits, MonthTens   int
	YearUnits, YearTens     int
}

// SynthDCF77 generates a DCF77-style pulse train covering `seconds` seconds.
// Each second starts with a LOW pulse of either 100 ms (bit=0) or 200 ms
// (bit=1). Second 59 is silent (no LOW pulse) — the minute mark. The frame
// structure (60 bits per minute) is simplified: only the BCD time fields
// are populated; everything else is zero.
func SynthDCF77(frame DCF77Frame, seconds int, sampleRate, vHigh, vLow float64) ([]float64, float64) {
	dtNs := 1e9 / sampleRate
	total := int(float64(seconds) * sampleRate)
	out := make([]float64, total)
	for i := range out {
		out[i] = vHigh
	}
	// Build the 60-bit frame. LSB-first BCD per field is DCF77 convention.
	bits := make([]int, 60)
	// bits[0]     0 (start of minute)
	// bits[20]    1 (start of time info)
	bits[20] = 1
	put := func(start, count, val int) {
		for k := 0; k < count; k++ {
			bits[start+k] = (val >> k) & 1
		}
	}
	put(21, 4, frame.MinuteUnits)
	put(25, 3, frame.MinuteTens)
	// parity for minutes (bit 28) — skip
	put(29, 4, frame.HourUnits)
	put(33, 2, frame.HourTens)
	// parity bit 35 — skip
	put(36, 4, frame.DayUnits)
	put(40, 2, frame.DayTens)
	put(42, 3, frame.DOWMon1Sun7)
	put(45, 4, frame.MonthUnits)
	put(49, 1, frame.MonthTens)
	put(50, 4, frame.YearUnits)
	put(54, 4, frame.YearTens)
	// bit 59 is silent (never written).

	secSmp := int(sampleRate)
	for s := 0; s < seconds; s++ {
		slot := s % 60
		if slot == 59 {
			continue
		}
		pulseLenS := 0.1
		if bits[slot] == 1 {
			pulseLenS = 0.2
		}
		pulseSmp := int(pulseLenS * sampleRate)
		from := s * secSmp
		to := from + pulseSmp
		if to > total {
			to = total
		}
		for i := from; i < to; i++ {
			out[i] = vLow
		}
	}
	return out, dtNs
}

// SynthIRIGB generates an IRIG-B DC-shift pulse train for 1 second.
// bit slots = 10 ms each, 100 slots / sec. Each bit starts with a LOW
// pulse whose width encodes the value:
//
//	2 ms = binary 0, 5 ms = binary 1, 8 ms = position identifier.
//
// The frame encodes seconds (bits 0-8), minutes (10-17), hours (20-26),
// day of year (30-41), year (50-57). Position identifiers sit at every
// 10th slot (P0..P9) plus Pr at the start.
func SynthIRIGB(sec, min, hour, doy, year int, sampleRate, vHigh, vLow float64) ([]float64, float64) {
	dtNs := 1e9 / sampleRate
	total := int(sampleRate) // exactly 1 second
	out := make([]float64, total)
	for i := range out {
		out[i] = vHigh
	}

	bits := make([]int, 100)
	// positions identifiers (8 ms pulses): slot 0 (Pr) and 9, 19, ..., 99
	// represented with sentinel 2 meaning "position identifier".
	bits[0] = 2 // Pr
	for k := 9; k < 100; k += 10 {
		bits[k] = 2
	}
	putBCD := func(start, count, val int) {
		for k := 0; k < count; k++ {
			if start+k >= 100 {
				return
			}
			bits[start+k] = (val >> k) & 1
		}
	}
	// seconds: units at bits 1-4, tens at bits 5-8 (3 bits tens, skip index-marker at 9).
	putBCD(1, 4, sec%10)
	putBCD(5, 4, sec/10)
	// minutes: units 10-13, tens 15-17 (14 is zero/reserved per IRIG-B)
	putBCD(10, 4, min%10)
	putBCD(15, 3, min/10)
	// hours: units 20-23, tens 25-26
	putBCD(20, 4, hour%10)
	putBCD(25, 2, hour/10)
	// day-of-year: units 30-33, tens 35-38, hundreds 40-41
	putBCD(30, 4, doy%10)
	putBCD(35, 4, (doy/10)%10)
	putBCD(40, 2, doy/100)
	// year (two digits): units 50-53, tens 55-58
	putBCD(50, 4, year%10)
	putBCD(55, 4, (year/10)%10)

	slotSmp := sampleRate * 0.010
	for k := 0; k < 100; k++ {
		startF := float64(k) * slotSmp
		var pulseMs float64
		switch bits[k] {
		case 0:
			pulseMs = 2
		case 1:
			pulseMs = 5
		case 2:
			pulseMs = 8
		}
		pulseSmp := int(pulseMs * 1e-3 * sampleRate)
		from := int(startF + 0.5)
		to := from + pulseSmp
		if to > total {
			to = total
		}
		for i := from; i < to; i++ {
			out[i] = vLow
		}
	}
	return out, dtNs
}

// SynthAFNOR_S87_500 approximates NF S87-500 with a 100 bits/sec DC-shift
// envelope matching IRIG-B timing (2/5/8 ms LOW pulses). The bit layout is
// different in practice but for classifier + structural testing we reuse
// the IRIG layout — the decoder re-tags it as AFNOR if the caller picked
// that protocol.
func SynthAFNOR_S87_500(sec, min, hour, doy, year int, sampleRate, vHigh, vLow float64) ([]float64, float64) {
	return SynthIRIGB(sec, min, hour, doy, year, sampleRate, vHigh, vLow)
}

// AddNoise adds gaussian noise with std deviation `sigma` mV in place.
// Fixed-seed PRNG so tests are reproducible.
func AddNoise(samples []float64, sigma float64, seed int64) {
	if sigma <= 0 {
		return
	}
	r := rand.New(rand.NewSource(seed))
	for i := range samples {
		samples[i] += r.NormFloat64() * sigma
	}
}
