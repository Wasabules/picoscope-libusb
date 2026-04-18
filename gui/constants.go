package main

const (
	maxSamplesSingle = 8064
	maxSamplesDual   = 3968
	// Driver uses the TB_CHAN lookup for tb 0..10 (SDK-verified) and a
	// progressive divide-by-2 extrapolation for 11..23. At very high tb each
	// block takes seconds, so streaming polling is sized accordingly.
	timebaseMax = 23
)

// Range name → ps_range_t enum value (must match driver).
var rangeEnumVal = map[string]int{
	"50mV": 2, "100mV": 3, "200mV": 4, "500mV": 5,
	"1V": 6, "2V": 7, "5V": 8, "10V": 9, "20V": 10,
}

// rangeNameToEnum is kept as a separate map alias for the calibration API
// to avoid coupling the calibration layer to the capture layer's map name.
var rangeNameToEnum = map[string]int{
	"50mV": 2, "100mV": 3, "200mV": 4, "500mV": 5,
	"1V": 6, "2V": 7, "5V": 8, "10V": 9, "20V": 10,
}

// Effective ADC full-scale range in mV (must match C driver RANGE_MV[]).
// 50mV uses digital scaling (same PGA as 5V, host windows to ±50 mV).
// 5V and 10V are calibrated empirically (bank 0 PGA gains).
var rangeMvVal = map[string]int{
	"50mV": 50, "100mV": 100, "200mV": 200, "500mV": 500,
	"1V": 1000, "2V": 2000, "5V": 3515, "10V": 9092, "20V": 20000,
}

// Siggen wave name → ps_wave_t enum value.
var waveTypeMap = map[string]int{
	"sine": 0, "square": 1, "triangle": 2,
	"rampup": 3, "rampdown": 4, "dc": 5,
}

// Streaming mode constants (must match ps_stream_mode_t in picoscope2204a.h).
const (
	streamModeFast   = 0
	streamModeNative = 1
	streamModeSDK    = 2
)

// Default ring buffer sizes per mode.
const (
	ringFast   = 1 << 20 // 1 M samples (~3 s at 330 kS/s)
	ringNative = 1 << 14 // 16 k samples (~160 s at 100 S/s)
	ringSDK    = 1 << 22 // 4 M samples (~4 s at 1 MS/s)
)
