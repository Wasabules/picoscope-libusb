package main

// Types exposed to the frontend via Wails JSON bindings.

type DeviceInfo struct {
	Connected bool   `json:"connected"`
	Serial    string `json:"serial"`
	CalDate   string `json:"calDate"`
}

type ChannelConfig struct {
	Enabled  bool   `json:"enabled"`
	Coupling string `json:"coupling"`
	Range    string `json:"range"`
}

type WaveformData struct {
	ChannelA   []float64 `json:"channelA"`
	ChannelB   []float64 `json:"channelB"`
	NumSamples int       `json:"numSamples"`
	Timebase   int       `json:"timebase"`
	TimebaseNs int       `json:"timebaseNs"`
	RangeMvA   int       `json:"rangeMvA"`
	RangeMvB   int       `json:"rangeMvB"`
}

type StreamStats struct {
	Blocks        uint64  `json:"blocks"`
	TotalSamples  uint64  `json:"totalSamples"`
	ElapsedS      float64 `json:"elapsedS"`
	SamplesPerSec float64 `json:"samplesPerSec"`
	BlocksPerSec  float64 `json:"blocksPerSec"`
	LastBlockMs   float64 `json:"lastBlockMs"`
	Mode          string  `json:"mode"`
}

type TimebaseInfo struct {
	Index      int    `json:"index"`
	IntervalNs int    `json:"intervalNs"`
	Label      string `json:"label"`
}

type Measurements struct {
	MinMv  float64 `json:"minMv"`
	MaxMv  float64 `json:"maxMv"`
	MeanMv float64 `json:"meanMv"`
	VppMv  float64 `json:"vppMv"`
}

// RawCaptureData holds the raw uint8 bytes returned by the device after a
// single capture. Useful for diagnosing dual-channel layout or PGA behaviour.
type RawCaptureData struct {
	Bytes      []int `json:"bytes"`
	NumBytes   int   `json:"numBytes"`
	NumSamples int   `json:"numSamples"`
	Timebase   int   `json:"timebase"`
	TimebaseNs int   `json:"timebaseNs"`
	Dual       bool  `json:"dual"`
}

// RangeCal holds the current (offset, gain) pair for a single range.
type RangeCal struct {
	Range    string  `json:"range"`
	OffsetMv float64 `json:"offset_mv"`
	Gain     float64 `json:"gain"`
}

// DecodeRequest is the wire-level shape the frontend sends to Decode().
type DecodeRequest struct {
	Protocol   string            `json:"protocol"`
	SamplesA   []float64         `json:"samplesA"`
	SamplesB   []float64         `json:"samplesB"`
	DtNs       float64           `json:"dtNs"`
	RangeMvA   float64           `json:"rangeMvA"`
	RangeMvB   float64           `json:"rangeMvB"`
	Config     map[string]any    `json:"config"`
	ChannelMap map[string]string `json:"channelMap"`
}
