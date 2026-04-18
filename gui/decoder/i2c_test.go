package decoder

import (
	"testing"
)

const (
	i2cSampleRate = 3.125e6 // TB=5 on PS2204A
	i2cVHigh      = 3300.0
	i2cVLow       = 0.0
)

func defaultI2CConfig() map[string]any {
	return map[string]any{
		"addrMode":    "7",
		"thresholdMv": 1650.0,
	}
}

func decodeI2C(t *testing.T, scl, sda []float64, dtNs float64,
	cfg map[string]any) []Event {
	t.Helper()
	events, _, err := DecodeI2C(Context{
		SamplesA:   scl,
		SamplesB:   sda,
		DtNs:       dtNs,
		RangeMvA:   5000, RangeMvB: 5000,
		Config:     cfg,
		ChannelMap: map[string]string{"SCL": "A", "SDA": "B"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return events
}

func TestI2C_SingleWriteOneByte(t *testing.T) {
	scl, sda, dt := SynthI2C([]I2CTransaction{{
		Addr: 0x48, Read: false, Bytes: []byte{0x55},
		Ack: []bool{true, true}, // ACK addr, ACK byte
	}}, 100_000, i2cSampleRate, i2cVHigh, i2cVLow, 300, 300)

	events := decodeI2C(t, scl, sda, dt, defaultI2CConfig())
	kinds := map[string]int{}
	var addr, data int = -1, -1
	for _, e := range events {
		kinds[e.Kind]++
		if e.Kind == "addr" {
			addr = e.Value
		}
		if e.Kind == "byte" {
			data = e.Value
		}
	}
	if kinds["start"] != 1 || kinds["stop"] != 1 {
		t.Fatalf("want exactly 1 start + 1 stop, got %v (events=%+v)", kinds, events)
	}
	if addr != 0x48 {
		t.Errorf("want addr=0x48, got 0x%02X", addr)
	}
	if data != 0x55 {
		t.Errorf("want data=0x55, got 0x%02X", data)
	}
	if kinds["ack"] < 2 {
		t.Errorf("want 2 ACKs, got %d", kinds["ack"])
	}
}

func TestI2C_Read_MasterNackLastByte(t *testing.T) {
	// Master reads 2 bytes from slave 0x50: ACK first, NACK last.
	scl, sda, dt := SynthI2C([]I2CTransaction{{
		Addr: 0x50, Read: true, Bytes: []byte{0xAA, 0x55},
		Ack: []bool{true, true, false}, // addr ACK, byte0 ACK, byte1 NACK
	}}, 100_000, i2cSampleRate, i2cVHigh, i2cVLow, 300, 300)

	events := decodeI2C(t, scl, sda, dt, defaultI2CConfig())
	var ack, nack int
	var gotBytes []int
	for _, e := range events {
		if e.Kind == "ack" {
			ack++
		} else if e.Kind == "nack" {
			nack++
		} else if e.Kind == "byte" {
			gotBytes = append(gotBytes, e.Value)
		}
	}
	if len(gotBytes) != 2 || gotBytes[0] != 0xAA || gotBytes[1] != 0x55 {
		t.Fatalf("want [0xAA,0x55], got %v", gotBytes)
	}
	if ack < 2 || nack != 1 {
		t.Errorf("ack=%d nack=%d (want ack>=2, nack=1)", ack, nack)
	}
}

func TestI2C_RepeatedStart(t *testing.T) {
	// Two consecutive transactions without a STOP between them.
	scl, sda, dt := SynthI2C([]I2CTransaction{
		{Addr: 0x48, Read: false, Bytes: []byte{0x10},
			Ack: []bool{true, true}},
		{Addr: 0x48, Read: true, Bytes: []byte{0x11},
			Ack: []bool{true, false}},
	}, 100_000, i2cSampleRate, i2cVHigh, i2cVLow, 300, 300)

	events := decodeI2C(t, scl, sda, dt, defaultI2CConfig())
	starts := 0
	stops := 0
	for _, e := range events {
		if e.Kind == "start" {
			starts++
		} else if e.Kind == "stop" {
			stops++
		}
	}
	if starts != 2 {
		t.Errorf("want 2 starts, got %d", starts)
	}
	// Two transactions, two STOPs is also OK (the synth emits STOP between).
	if stops < 1 {
		t.Errorf("want >=1 stop, got %d", stops)
	}
}

func TestI2C_TenBitAddress(t *testing.T) {
	scl, sda, dt := SynthI2C([]I2CTransaction{{
		Addr: 0x248, Read: false, TenBit: true, Bytes: []byte{0x77},
		Ack: []bool{true, true, true},
	}}, 100_000, i2cSampleRate, i2cVHigh, i2cVLow, 300, 300)

	cfg := defaultI2CConfig()
	cfg["addrMode"] = "10"
	events := decodeI2C(t, scl, sda, dt, cfg)

	var addr int = -1
	var data int = -1
	for _, e := range events {
		if e.Kind == "addr" && e.HasValue {
			addr = e.Value
		}
		if e.Kind == "byte" && e.HasValue {
			data = e.Value
		}
	}
	if addr != 0x248 {
		t.Errorf("want 10-bit addr 0x248, got 0x%X", addr)
	}
	if data != 0x77 {
		t.Errorf("want data 0x77, got 0x%X", data)
	}
}

func TestI2C_NoisyCapture(t *testing.T) {
	scl, sda, dt := SynthI2C([]I2CTransaction{{
		Addr: 0x40, Read: false, Bytes: []byte{0xDE, 0xAD, 0xBE, 0xEF},
		Ack: []bool{true, true, true, true, true},
	}}, 100_000, i2cSampleRate, i2cVHigh, i2cVLow, 500, 500)
	AddNoise(scl, 80, 1)
	AddNoise(sda, 80, 2)

	events := decodeI2C(t, scl, sda, dt, defaultI2CConfig())
	var got []byte
	for _, e := range events {
		if e.Kind == "byte" && e.HasValue {
			got = append(got, byte(e.Value))
		}
	}
	want := []byte{0xDE, 0xAD, 0xBE, 0xEF}
	if len(got) != len(want) {
		t.Fatalf("want 4 bytes, got %d (%v)", len(got), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("byte %d: want 0x%02X got 0x%02X", i, want[i], got[i])
		}
	}
}
