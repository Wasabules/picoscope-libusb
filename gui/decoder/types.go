// Package decoder hosts the protocol decoders. It runs in the Go backend
// so the WebKit UI thread is never blocked by signal-processing work.
//
// The public contract is a single Decode() entry point per protocol that
// takes analog samples (mV) plus a free-form config map and returns a
// slice of events that the frontend renders as a log.
package decoder

// Event is one decoded symbol / frame / error. Timestamps are relative to
// the first sample of the window (sample 0 == 0 ns).
type Event struct {
	TNs        float64 `json:"t_ns"`
	TEndNs     float64 `json:"t_end_ns,omitempty"`
	Kind       string  `json:"kind"`                 // "byte" | "error" | ...
	Level      string  `json:"level,omitempty"`      // "info" | "warn" | "error"
	Value      int     `json:"value"`                // 0..255 for byte
	HasValue   bool    `json:"has_value"`            // distinguish 0 vs unset
	Annotation string  `json:"annotation,omitempty"` // short tag ("55", "PARITY")
	Text       string  `json:"text,omitempty"`       // human-readable line
}

// Context bundles the inputs a decoder needs. `ChannelMap` maps a role name
// ("DATA", "SCL", "SDA", "CLK", ...) to "A" or "B".
type Context struct {
	SamplesA   []float64
	SamplesB   []float64
	DtNs       float64
	RangeMvA   float64
	RangeMvB   float64
	Config     map[string]any
	ChannelMap map[string]string
}

// Result is what the frontend receives. DebugLog captures decoder-internal
// notes for the JS side to display (zero-cost when empty).
type Result struct {
	Protocol string   `json:"protocol"`
	Events   []Event  `json:"events"`
	Error    string   `json:"error,omitempty"`
	DebugLog []string `json:"debug_log,omitempty"`
	DecodeMs float64  `json:"decode_ms"`
}
