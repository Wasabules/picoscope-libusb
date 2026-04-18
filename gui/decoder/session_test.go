package decoder

import (
	"io"
	"log"
	"testing"
)

// Silence the package-level log.Printf during tests.
func silenceLogs() {
	log.SetOutput(io.Discard)
}

func sessionBytes(events []Event) []byte {
	out := make([]byte, 0, len(events))
	for _, e := range events {
		if e.Kind == "byte" && e.HasValue {
			out = append(out, byte(e.Value))
		}
	}
	return out
}

// Split a flat sample slice into N roughly-equal chunks — emulates how
// the streaming goroutine serves fixed-size blocks to the session.
func chunk(samples []float64, blockSize int) [][]float64 {
	var out [][]float64
	for i := 0; i < len(samples); i += blockSize {
		end := i + blockSize
		if end > len(samples) {
			end = len(samples)
		}
		out = append(out, samples[i:end])
	}
	return out
}

func TestSession_SingleBlock(t *testing.T) {
	silenceLogs()
	samples, dtNs := SynthUART([]byte("OK"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	defer s.Close()

	got := sessionBytes(s.Feed(samples, nil, dtNs, 5000, 0))
	if string(got) != "OK" {
		t.Fatalf("want OK, got %q", got)
	}
}

func TestSession_CrossBlockFrame(t *testing.T) {
	silenceLogs()
	// "Hello" at 9600 baud ≈ 5.2 ms = 16280 samples at 3.125 MSPS.
	// Split into three uneven blocks so most frames straddle boundaries.
	msg := []byte("Hello, stream!")
	samples, dtNs := SynthUART(msg, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)

	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	defer s.Close()

	var got []byte
	for _, blk := range chunk(samples, 4000) {
		got = append(got, sessionBytes(s.Feed(blk, nil, dtNs, 5000, 0))...)
	}
	if string(got) != string(msg) {
		t.Fatalf("want %q, got %q", msg, got)
	}
}

func TestSession_NoDuplicatesAcrossBlocks(t *testing.T) {
	silenceLogs()
	// Long message, many small blocks. Every byte must appear exactly once.
	msg := []byte("The streaming decoder must not double-count any byte even " +
		"though the tail buffer overlaps consecutive Feed() calls.")
	samples, dtNs := SynthUART(msg, 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)

	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	defer s.Close()

	var got []byte
	for _, blk := range chunk(samples, 1500) {
		got = append(got, sessionBytes(s.Feed(blk, nil, dtNs, 5000, 0))...)
	}
	if string(got) != string(msg) {
		t.Fatalf("length got=%d want=%d\nwant %q\ngot  %q",
			len(got), len(msg), msg, got)
	}
}

func TestSession_AbsoluteTimestampsMonotonic(t *testing.T) {
	silenceLogs()
	samples, dtNs := SynthUART([]byte("0123456789"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)

	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	defer s.Close()

	var all []Event
	for _, blk := range chunk(samples, 3000) {
		all = append(all, s.Feed(blk, nil, dtNs, 5000, 0)...)
	}
	if len(all) != 10 {
		t.Fatalf("want 10 events, got %d", len(all))
	}
	// Absolute timestamps must be strictly increasing across the session.
	for i := 1; i < len(all); i++ {
		if all[i].TNs <= all[i-1].TNs {
			t.Fatalf("timestamps not monotonic at %d: %.0f ≤ %.0f",
				i, all[i].TNs, all[i-1].TNs)
		}
	}
	// Each byte should be ~ (1 frame) apart at 9600 baud = 1.042 ms.
	expectedGap := 1e9 / 9600.0 * 10
	for i := 1; i < len(all); i++ {
		gap := all[i].TNs - all[i-1].TNs
		if gap < expectedGap*0.9 || gap > expectedGap*1.1 {
			t.Fatalf("gap at %d = %.0f ns, expected ≈ %.0f",
				i, gap, expectedGap)
		}
	}
}

func TestSession_IdleBetweenBursts(t *testing.T) {
	silenceLogs()
	// First burst, then several idle-only blocks, then second burst.
	burst1, dtNs := SynthUART([]byte("FIRST"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	burst2, _ := SynthUART([]byte("SECOND"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	idle := make([]float64, 10000)
	for i := range idle {
		idle[i] = testVHigh
	}

	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	defer s.Close()

	var got []byte
	feed := func(blk []float64) {
		got = append(got, sessionBytes(s.Feed(blk, nil, dtNs, 5000, 0))...)
	}
	feed(burst1)
	for i := 0; i < 5; i++ {
		feed(idle)
	}
	feed(burst2)

	if string(got) != "FIRSTSECOND" {
		t.Fatalf("want FIRSTSECOND, got %q", got)
	}
}

func TestSession_ClosedRejectsFeeds(t *testing.T) {
	silenceLogs()
	s := NewSession("uart", defaultConfig(),
		map[string]string{"DATA": "A"})
	s.Close()
	samples, dtNs := SynthUART([]byte("X"), 9600, testSampleRate,
		testVHigh, testVLow, 500, 500)
	if ev := s.Feed(samples, nil, dtNs, 5000, 0); len(ev) != 0 {
		t.Fatalf("closed session should reject feed, got %d events", len(ev))
	}
}

// A realistic Fast-streaming scenario: 8064-sample blocks arriving at the
// poll rate, containing a continuous stream of printable ASCII at 115200
// baud (one of the highest common rates).
func TestSession_FastStreaming115200(t *testing.T) {
	silenceLogs()
	msg := []byte("The quick brown fox jumps over the lazy dog 0123456789!?")
	samples, dtNs := SynthUART(msg, 115200, testSampleRate,
		testVHigh, testVLow, 200, 200)

	cfg := defaultConfig()
	cfg["baud"] = 115200.0
	s := NewSession("uart", cfg, map[string]string{"DATA": "A"})
	defer s.Close()

	var got []byte
	for _, blk := range chunk(samples, 8064) {
		got = append(got, sessionBytes(s.Feed(blk, nil, dtNs, 5000, 0))...)
	}
	if string(got) != string(msg) {
		t.Fatalf("fast streaming 115200: want %q, got %q", msg, got)
	}
}
