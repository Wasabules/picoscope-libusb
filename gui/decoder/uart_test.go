package decoder

import (
	"fmt"
	"math"
	"testing"
)

// Common synth parameters used across tests. 3.125 MSPS corresponds to the
// PS2204A's TB=5 (320 ns/sample). `thresholdMv=1500` puts the Schmitt mid-
// level halfway between vLow=0 and vHigh=3300 (TTL 3V3).
const (
	testSampleRate = 3.125e6
	testVHigh      = 3300.0
	testVLow       = 0.0
	testThreshold  = 1500.0
)

func defaultConfig() map[string]any {
	return map[string]any{
		"baud":        9600.0,
		"dataBits":    8.0,
		"parity":      "none",
		"stopBits":    1.0,
		"lsbFirst":    true,
		"thresholdMv": testThreshold,
	}
}

func mustDecodeBytes(t *testing.T, samples []float64, dtNs float64,
	cfg map[string]any) []byte {
	t.Helper()
	events, dbg, err := DecodeUART(Context{
		SamplesA:   samples,
		DtNs:       dtNs,
		RangeMvA:   5000,
		Config:     cfg,
		ChannelMap: map[string]string{"DATA": "A"},
	})
	if err != nil {
		t.Fatalf("decode error: %v (dbg=%v)", err, dbg)
	}
	out := make([]byte, 0, len(events))
	for _, e := range events {
		if e.Kind == "byte" && e.HasValue {
			out = append(out, byte(e.Value))
		}
	}
	return out
}

func TestDecodeUART_SingleByte0x55(t *testing.T) {
	samples, dtNs := SynthUART([]byte{0x55}, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	if len(got) != 1 || got[0] != 0x55 {
		t.Fatalf("want [0x55], got %v", got)
	}
}

func TestDecodeUART_AllBytes0To255(t *testing.T) {
	// Every possible byte value, transmitted once.
	payload := make([]byte, 256)
	for i := 0; i < 256; i++ {
		payload[i] = byte(i)
	}
	samples, dtNs := SynthUART(payload, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	if len(got) != 256 {
		t.Fatalf("want 256 bytes, got %d", len(got))
	}
	for i, b := range got {
		if int(b) != i {
			t.Fatalf("byte %d: want 0x%02x, got 0x%02x", i, i, b)
		}
	}
}

func TestDecodeUART_BackToBackStream(t *testing.T) {
	msg := []byte("The quick brown fox jumps over the lazy dog.")
	samples, dtNs := SynthUART(msg, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	if string(got) != string(msg) {
		t.Fatalf("want %q, got %q", msg, got)
	}
}

func TestDecodeUART_VariousBauds(t *testing.T) {
	msg := []byte("COUCOU A TOUS")
	bauds := []float64{1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}

	for _, baud := range bauds {
		t.Run(fmt.Sprintf("baud=%d", int(baud)), func(t *testing.T) {
			// Need enough sample rate that bit period ≥ ~6 samples.
			// 3.125 MSPS handles up to ~500 kbaud easily.
			samples, dtNs := SynthUART(msg, baud, testSampleRate,
				testVHigh, testVLow, 500, 500)
			cfg := defaultConfig()
			cfg["baud"] = baud
			got := mustDecodeBytes(t, samples, dtNs, cfg)
			if string(got) != string(msg) {
				t.Fatalf("baud=%g: want %q, got %q", baud, msg, got)
			}
		})
	}
}

func TestDecodeUART_VariousSampleRates(t *testing.T) {
	msg := []byte("Hi!")
	// The PS2204A's timebases: dt_ns = 10 * 2^tb → sample rates 100 MSPS,
	// 50 MSPS, 25 MSPS, 12.5 MSPS, 6.25 MSPS, 3.125 MSPS, 1.5625 MSPS, ...
	// Only rates that leave ≥ 4 samples/bit are realistic for 9600 baud.
	for tb := 0; tb <= 10; tb++ {
		dtNs := 10.0 * float64(int(1)<<tb)
		sampleRate := 1e9 / dtNs
		bitSamples := sampleRate / 9600.0
		if bitSamples < 4 {
			continue
		}
		t.Run(fmt.Sprintf("TB=%d(dt=%.0fns)", tb, dtNs), func(t *testing.T) {
			samples, actualDtNs := SynthUART(msg, 9600, sampleRate,
				testVHigh, testVLow, 500, 500)
			got := mustDecodeBytes(t, samples, actualDtNs, defaultConfig())
			if string(got) != string(msg) {
				t.Fatalf("TB=%d (dt=%.0f ns, bitSmp=%.1f): want %q, got %q",
					tb, dtNs, bitSamples, msg, got)
			}
		})
	}
}

func TestDecodeUART_RejectsMidByteCaptureStart(t *testing.T) {
	// Scope catches a trace mid-transmission: drop the first N samples so
	// the buffer starts inside a data bit, not on idle. The decoder should
	// still recover once it hits a real idle→start boundary.
	samples, dtNs := SynthUART([]byte("HELLO"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	// Chop off the first 400 samples (128 µs) → lands inside 'H' frame.
	trimmed := samples[400:]
	got := mustDecodeBytes(t, trimmed, dtNs, defaultConfig())
	// We lose 'H' but subsequent bytes must decode cleanly.
	if len(got) == 0 || got[len(got)-1] != 'O' {
		t.Fatalf("want trailing 'O' after resync, got %q (len=%d)", got, len(got))
	}
	// Tail is contained in "HELLO" as-is.
	if !containsSubsequence("HELLO", string(got)) {
		t.Fatalf("decoded %q is not a tail of HELLO", got)
	}
}

func containsSubsequence(full, tail string) bool {
	for i := 0; i <= len(full)-len(tail); i++ {
		if full[i:i+len(tail)] == tail {
			return true
		}
	}
	return false
}

func TestDecodeUART_WithNoise(t *testing.T) {
	msg := []byte("noise-test")
	samples, dtNs := SynthUART(msg, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	AddNoise(samples, 200, 42) // 200 mV RMS noise on 3.3 V swing ≈ 6 %
	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	if string(got) != string(msg) {
		t.Fatalf("want %q, got %q (noise should not break framing)", msg, got)
	}
}

func TestDecodeUART_EvenParity(t *testing.T) {
	// Hand-craft a bit stream with even parity bits.
	synth := func(data []byte) []float64 {
		dtNs := 1e9 / testSampleRate
		bitSmp := testSampleRate / 9600
		// pre(500) + per-byte 11 slots (start + 8 data + parity + stop)
		total := 500 + int(float64(len(data))*bitSmp*11+0.5) + 500
		out := make([]float64, total)
		for i := range out {
			out[i] = testVHigh
		}
		for bi, b := range data {
			base := 500.0 + float64(bi)*bitSmp*11
			bits := make([]float64, 11)
			bits[0] = testVLow
			ones := 0
			for k := 0; k < 8; k++ {
				if (b>>k)&1 == 1 {
					bits[1+k] = testVHigh
					ones++
				} else {
					bits[1+k] = testVLow
				}
			}
			// Even parity: set parity bit so total ones (data+parity) is even.
			if ones%2 == 1 {
				bits[9] = testVHigh
			} else {
				bits[9] = testVLow
			}
			bits[10] = testVHigh
			for k := 0; k < 11; k++ {
				s := int(base + float64(k)*bitSmp + 0.5)
				e := int(base + float64(k+1)*bitSmp + 0.5)
				for i := s; i < e && i < total; i++ {
					out[i] = bits[k]
				}
			}
		}
		_ = dtNs
		return out
	}
	samples := synth([]byte{'A', 'B', 'C'})
	cfg := defaultConfig()
	cfg["parity"] = "even"
	got := mustDecodeBytes(t, samples, 1e9/testSampleRate, cfg)
	if string(got) != "ABC" {
		t.Fatalf("even parity: want ABC, got %q", got)
	}
}

// Field reproducer: 3.3 V TTL UART captured on a too-narrow scope range
// (±1 V). Raw samples clip at ±1000 mV so the fixed 1500 mV Schmitt level
// is never crossed. The auto-threshold picks the midpoint of the actual
// signal range and recovers the bytes.
func TestDecodeUART_ClippedSignalAutoThreshold(t *testing.T) {
	// Generate a 3.3 V swing, then clip to a 1 V scope range.
	samples, dtNs := SynthUART([]byte("Hello"), 9600, testSampleRate,
		3300, 0, 500, 500)
	for i, v := range samples {
		if v > 1000 {
			samples[i] = 1000
		} else if v < -1000 {
			samples[i] = -1000
		}
	}

	// Fixed threshold at 1500 mV: never crossed → no bytes decoded.
	fixedCfg := defaultConfig()
	fixedCfg["autoThreshold"] = false
	gotFixed := mustDecodeBytes(t, samples, dtNs, fixedCfg)
	if string(gotFixed) == "Hello" {
		t.Fatalf("sanity: fixed threshold was not supposed to work on clipped signal, got %q", gotFixed)
	}

	// Auto threshold (default true): recovers the bytes.
	autoCfg := defaultConfig()
	gotAuto := mustDecodeBytes(t, samples, dtNs, autoCfg)
	if string(gotAuto) != "Hello" {
		t.Fatalf("auto threshold failed on clipped 3.3 V TTL: want Hello, got %q",
			gotAuto)
	}
}

// Idle line with moderate noise must not produce spurious bytes — the real
// field bug was an auto-threshold with vanishing hysteresis picking up
// noise as thousands of 0x00 frames.
func TestDecodeUART_NoisyIdleNoSpurious(t *testing.T) {
	dtNs := 1e9 / testSampleRate
	// 40 k samples of 3.3 V idle + 150 mV RMS noise. No real traffic at all.
	n := 40000
	samples := make([]float64, n)
	for i := range samples {
		samples[i] = testVHigh
	}
	AddNoise(samples, 150, 0xBEEF)

	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	if len(got) > 0 {
		t.Fatalf("idle line must decode 0 bytes, got %d: %v", len(got), got)
	}
}

// Slow analog signal (e.g. a sine or mains hum) triggers auto-threshold
// fallback rather than hallucinating UART framing.
func TestDecodeUART_SlowAnalogNoSpurious(t *testing.T) {
	dtNs := 1e9 / testSampleRate
	n := 40000
	samples := make([]float64, n)
	// 50 Hz 1.5 V peak (3 V pp), centered at 1.65 V — the worst case for
	// auto-threshold: big swing but smooth, no rails to latch onto.
	for i := range samples {
		t := float64(i) * dtNs * 1e-9
		samples[i] = 1650 + 1500*math.Sin(2*math.Pi*50*t)
	}
	got := mustDecodeBytes(t, samples, dtNs, defaultConfig())
	// At 50 Hz with ~10 Hz-ish effective bit-rate matching, a handful of
	// false frames is plausible but an "infinite stream of 0x00" is not.
	if len(got) > 3 {
		t.Fatalf("slow analog signal should not decode as UART: got %d bytes", len(got))
	}
}

func TestDecodeUART_Undersampled(t *testing.T) {
	// Bit period must be ≥ 2 samples; here we deliberately undersample.
	// 100 kbaud on 50 kS/s → 0.5 sample/bit.
	samples, dtNs := SynthUART([]byte{0x55}, 100000, 50000,
		testVHigh, testVLow, 500, 500)
	cfg := defaultConfig()
	cfg["baud"] = 100000.0
	events, _, err := DecodeUART(Context{
		SamplesA: samples, DtNs: dtNs,
		Config: cfg, ChannelMap: map[string]string{"DATA": "A"},
	})
	if err != nil {
		t.Fatalf("decode errored, expected undersampled event: %v", err)
	}
	// First event should be an undersampled error.
	if len(events) == 0 || events[0].Annotation != "UNDERSAMPLED" {
		t.Fatalf("expected UNDERSAMPLED event, got %+v", events)
	}
}

func BenchmarkDecodeUART_8kSamples(b *testing.B) {
	samples, dtNs := SynthUART([]byte("Hello, World!"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	// Pad/trim to 8k samples.
	if len(samples) < 8000 {
		pad := make([]float64, 8000-len(samples))
		for i := range pad {
			pad[i] = testVHigh
		}
		samples = append(samples, pad...)
	}
	samples = samples[:8000]
	cfg := defaultConfig()
	ctx := Context{
		SamplesA: samples, DtNs: dtNs,
		Config: cfg, ChannelMap: map[string]string{"DATA": "A"},
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		DecodeUART(ctx)
	}
}
