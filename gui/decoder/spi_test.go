package decoder

import (
	"testing"
)

const (
	spiSampleRate = 3.125e6
	spiVHigh      = 3300.0
	spiVLow       = 0.0
)

// decodeSPI2 runs the decoder with SCLK on A and MOSI on B (no MISO/CS).
// Two-line tests let us exercise the core bit-packing without worrying about
// CS framing or the 2-channel scope limitation.
func decodeSPI2(t *testing.T, sclk, mosi []float64, dtNs float64,
	cfg map[string]any) []Event {
	t.Helper()
	events, _, err := DecodeSPI(Context{
		SamplesA: sclk, SamplesB: mosi, DtNs: dtNs,
		RangeMvA: 5000, RangeMvB: 5000,
		Config:     cfg,
		ChannelMap: map[string]string{"SCLK": "A", "MOSI": "B"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return events
}

func defaultSPIConfig() map[string]any {
	return map[string]any{
		"bits":        8.0,
		"cpol":        0.0,
		"cpha":        0.0,
		"msbFirst":    true,
		"csActiveLow": true,
		"thresholdMv": 1650.0,
	}
}

func TestSPI_Mode0_MSBFirst_SingleByte(t *testing.T) {
	sclk, mosi, _, _, dt := SynthSPI([]byte{0x5A}, nil,
		1_000_000, spiSampleRate, spiVHigh, spiVLow, 0, 0, true, 300, 300)

	events := decodeSPI2(t, sclk, mosi, dt, defaultSPIConfig())
	var got []int
	for _, e := range events {
		if e.Kind == "mosi" {
			got = append(got, e.Value)
		}
	}
	if len(got) != 1 || got[0] != 0x5A {
		t.Fatalf("want [0x5A], got %v (events=%+v)", got, events)
	}
}

func TestSPI_Mode0_LSBFirst(t *testing.T) {
	// SynthSPI with msbFirst=false shifts D0 out first. Decoder with
	// lsbFirst=true should recover the same byte.
	sclk, mosi, _, _, dt := SynthSPI([]byte{0x81}, nil,
		1_000_000, spiSampleRate, spiVHigh, spiVLow, 0, 0, false, 300, 300)

	cfg := defaultSPIConfig()
	cfg["msbFirst"] = false
	events := decodeSPI2(t, sclk, mosi, dt, cfg)
	var got []int
	for _, e := range events {
		if e.Kind == "mosi" {
			got = append(got, e.Value)
		}
	}
	if len(got) != 1 || got[0] != 0x81 {
		t.Fatalf("want [0x81], got %v", got)
	}
}

func TestSPI_Mode0_MultipleBytes(t *testing.T) {
	payload := []byte{0xDE, 0xAD, 0xBE, 0xEF}
	sclk, mosi, _, _, dt := SynthSPI(payload, nil,
		1_000_000, spiSampleRate, spiVHigh, spiVLow, 0, 0, true, 300, 300)

	events := decodeSPI2(t, sclk, mosi, dt, defaultSPIConfig())
	var got []byte
	for _, e := range events {
		if e.Kind == "mosi" {
			got = append(got, byte(e.Value))
		}
	}
	if len(got) != len(payload) {
		t.Fatalf("want %d bytes, got %d (%v)", len(payload), len(got), got)
	}
	for i := range payload {
		if got[i] != payload[i] {
			t.Errorf("byte %d: want 0x%02X got 0x%02X", i, payload[i], got[i])
		}
	}
}

func TestSPI_Mode3(t *testing.T) {
	// CPOL=1, CPHA=1 — sample on rising (second edge out of idle HIGH).
	sclk, mosi, _, _, dt := SynthSPI([]byte{0xA5}, nil,
		1_000_000, spiSampleRate, spiVHigh, spiVLow, 1, 1, true, 300, 300)
	cfg := defaultSPIConfig()
	cfg["cpol"] = 1.0
	cfg["cpha"] = 1.0
	events := decodeSPI2(t, sclk, mosi, dt, cfg)
	var got []int
	for _, e := range events {
		if e.Kind == "mosi" {
			got = append(got, e.Value)
		}
	}
	if len(got) != 1 || got[0] != 0xA5 {
		t.Fatalf("want [0xA5], got %v (events=%+v)", got, events)
	}
}

func TestSPI_Noisy(t *testing.T) {
	payload := []byte{0x11, 0x22, 0x33}
	sclk, mosi, _, _, dt := SynthSPI(payload, nil,
		1_000_000, spiSampleRate, spiVHigh, spiVLow, 0, 0, true, 300, 300)
	AddNoise(sclk, 80, 1)
	AddNoise(mosi, 80, 2)

	events := decodeSPI2(t, sclk, mosi, dt, defaultSPIConfig())
	var got []byte
	for _, e := range events {
		if e.Kind == "mosi" {
			got = append(got, byte(e.Value))
		}
	}
	if len(got) != len(payload) {
		t.Fatalf("want %d bytes, got %d (%v)", len(payload), len(got), got)
	}
	for i := range payload {
		if got[i] != payload[i] {
			t.Errorf("byte %d: want 0x%02X got 0x%02X", i, payload[i], got[i])
		}
	}
}
