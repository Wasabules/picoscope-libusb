package decoder

import (
	"fmt"
	"log"
	"time"
)

// Descriptor is the metadata the frontend uses to populate its protocol
// selector. Keep it lightweight — the channel roles + config fields are all
// the JS side needs to render the UI.
type Descriptor struct {
	ID          string      `json:"id"`
	Name        string      `json:"name"`
	Description string      `json:"description"`
	Channels    []Channel   `json:"channels"`
	Config      []ConfigKey `json:"config"`
}

type Channel struct {
	Role     string `json:"role"`
	Required bool   `json:"required"`
	Help     string `json:"help,omitempty"`
}

type ConfigKey struct {
	Key     string         `json:"key"`
	Label   string         `json:"label"`
	Type    string         `json:"type"` // "number" | "select" | "boolean"
	Default any            `json:"default"`
	Options []SelectOption `json:"options,omitempty"`
	Min     *float64       `json:"min,omitempty"`
	Max     *float64       `json:"max,omitempty"`
	Step    *float64       `json:"step,omitempty"`
	Unit    string         `json:"unit,omitempty"`
	Help    string         `json:"help,omitempty"`
}

type SelectOption struct {
	Value any    `json:"value"`
	Label string `json:"label"`
}

// List enumerates every decoder the frontend can pick.
func List() []Descriptor {
	return []Descriptor{
		{
			ID:          "uart",
			Name:        "UART",
			Description: "Asynchronous serial (RS-232 / TTL). One signal, idle HIGH.",
			Channels: []Channel{
				{Role: "DATA", Required: true, Help: "TX or RX line; idle HIGH."},
			},
			Config: []ConfigKey{
				{Key: "baud", Label: "Baud rate", Type: "number", Default: 9600, Unit: "bd"},
				{Key: "dataBits", Label: "Data bits", Type: "select", Default: 8,
					Options: []SelectOption{{Value: 7, Label: "7"}, {Value: 8, Label: "8"}}},
				{Key: "parity", Label: "Parity", Type: "select", Default: "none",
					Options: []SelectOption{
						{Value: "none", Label: "None"},
						{Value: "even", Label: "Even"},
						{Value: "odd", Label: "Odd"},
					}},
				{Key: "stopBits", Label: "Stop bits", Type: "select", Default: 1,
					Options: []SelectOption{{Value: 1, Label: "1"}, {Value: 2, Label: "2"}}},
				{Key: "lsbFirst", Label: "LSB first", Type: "boolean", Default: true},
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean",
					Default: true,
					Help: "Derive the Schmitt level from the captured signal " +
						"(min/max); falls back to the fixed mV below when idle."},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number",
					Default: 1500,
					Help:    "Fallback mid-level when auto threshold is off or signal is idle."},
				{Key: "sof", Label: "SOF byte", Type: "text", Default: "",
					Help: "Start-of-frame delimiter (hex, e.g. 'AA' or '0x7E'). " +
						"Leave empty to disable packet framing."},
				{Key: "eof", Label: "EOF byte", Type: "text", Default: "",
					Help: "End-of-frame delimiter. Packets are only emitted when " +
						"both SOF and EOF are set — noise that never pairs up is " +
						"filtered out of the packet view."},
			},
		},
		{
			ID:          "i2c",
			Name:        "I²C",
			Description: "Two-wire synchronous serial. SCL + SDA, open-drain.",
			Channels: []Channel{
				{Role: "SCL", Required: true, Help: "Clock line."},
				{Role: "SDA", Required: true, Help: "Data line."},
			},
			Config: []ConfigKey{
				{Key: "addrMode", Label: "Address mode", Type: "select",
					Default: "7",
					Options: []SelectOption{
						{Value: "7", Label: "7-bit"},
						{Value: "10", Label: "10-bit"},
					}},
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500,
					Help: "Fallback level when auto is off or the signal is idle."},
			},
		},
		{
			ID:          "spi",
			Name:        "SPI",
			Description: "4-wire synchronous. Map SCLK + any one of MOSI/MISO (the scope has 2 channels).",
			Channels: []Channel{
				{Role: "SCLK", Required: true, Help: "Clock."},
				{Role: "MOSI", Required: false, Help: "Master-Out line."},
				{Role: "MISO", Required: false, Help: "Master-In line."},
				{Role: "CS", Required: false, Help: "Chip-select (optional)."},
			},
			Config: []ConfigKey{
				{Key: "bits", Label: "Word bits", Type: "number", Default: 8},
				{Key: "cpol", Label: "CPOL", Type: "select", Default: 0,
					Options: []SelectOption{
						{Value: 0, Label: "0 (idle LOW)"},
						{Value: 1, Label: "1 (idle HIGH)"},
					}},
				{Key: "cpha", Label: "CPHA", Type: "select", Default: 0,
					Options: []SelectOption{
						{Value: 0, Label: "0 (sample leading)"},
						{Value: 1, Label: "1 (sample trailing)"},
					}},
				{Key: "msbFirst", Label: "MSB first", Type: "boolean", Default: true},
				{Key: "csActiveLow", Label: "CS active LOW", Type: "boolean", Default: true},
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
		{
			ID:          "dcf77",
			Name:        "DCF77",
			Description: "Germany 77.5 kHz longwave time code (AM envelope).",
			Channels: []Channel{
				{Role: "DATA", Required: true, Help: "Digitised envelope (TTL)."},
			},
			Config: []ConfigKey{
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
		{
			ID:          "irig",
			Name:        "IRIG-B (DC-shift)",
			Description: "100 bits/s DC-shift time code. Auto-detects B000/B004.",
			Channels: []Channel{
				{Role: "DATA", Required: true, Help: "DC-shift envelope (TTL)."},
			},
			Config: []ConfigKey{
				{Key: "variant", Label: "Variant", Type: "select", Default: "auto",
					Options: []SelectOption{
						{Value: "auto", Label: "Auto-detect"},
						{Value: "B000", Label: "B000 (no SBS, no CF)"},
						{Value: "B004", Label: "B004 (SBS present)"},
					}},
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
		{
			ID:          "afnor",
			Name:        "AFNOR NF S87-500",
			Description: "French time code (IRIG-B-compatible carrier, FR-specific fields).",
			Channels: []Channel{
				{Role: "DATA", Required: true, Help: "DC-shift envelope (TTL)."},
			},
			Config: []ConfigKey{
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
		{
			ID:          "timecode_auto",
			Name:        "Time-code auto-detect",
			Description: "Fingerprints the pulse train and dispatches to DCF77 / IRIG-B / AFNOR.",
			Channels: []Channel{
				{Role: "DATA", Required: true, Help: "Digitised envelope (TTL)."},
			},
			Config: []ConfigKey{
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
		{
			ID:          "can",
			Name:        "CAN",
			Description: "Single-wire CAN view (recessive HIGH, dominant LOW). Handles 11/29-bit IDs.",
			Channels: []Channel{
				{Role: "CAN", Required: true, Help: "CAN_H or differential probe."},
			},
			Config: []ConfigKey{
				{Key: "bitRate", Label: "Bit rate", Type: "number", Default: 500000, Unit: "bps"},
				{Key: "samplePoint", Label: "Sample point", Type: "number", Default: 0.625,
					Help: "Fraction into the bit (0..1) where the level is sampled."},
				{Key: "autoThreshold", Label: "Auto threshold", Type: "boolean", Default: true},
				{Key: "thresholdMv", Label: "Threshold mV", Type: "number", Default: 1500},
			},
		},
	}
}

// Decode dispatches to the protocol-specific implementation. The Result is
// always populated (never nil) so the frontend has stable shape to render.
func Decode(protocol string, ctx Context) Result {
	t0 := time.Now()
	res := Result{Protocol: protocol}
	var events []Event
	var dbg []string
	var err error

	switch protocol {
	case "uart":
		events, dbg, err = DecodeUART(ctx)
	case "i2c":
		events, dbg, err = DecodeI2C(ctx)
	case "spi":
		events, dbg, err = DecodeSPI(ctx)
	case "can":
		events, dbg, err = DecodeCAN(ctx)
	case "dcf77":
		events, dbg, err = DecodeDCF77(ctx)
	case "irig", "irigb":
		events, dbg, err = DecodeIRIG(ctx)
	case "afnor":
		events, dbg, err = DecodeAFNOR(ctx)
	case "timecode_auto":
		events, dbg, err = decodeTimecodeAuto(ctx)
	default:
		err = fmt.Errorf("unknown protocol %q", protocol)
	}
	res.Events = events
	res.DebugLog = dbg
	res.DecodeMs = float64(time.Since(t0).Microseconds()) / 1000.0
	if err != nil {
		res.Error = err.Error()
		log.Printf("[decoder.%s] error: %v", protocol, err)
	}
	if len(dbg) > 0 {
		for _, line := range dbg {
			log.Printf("[decoder.%s] %s", protocol, line)
		}
	}
	log.Printf("[decoder.%s] %.2f ms → %d events", protocol, res.DecodeMs, len(events))
	return res
}
