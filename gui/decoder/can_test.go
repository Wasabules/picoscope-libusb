package decoder

import (
	"testing"
)

const (
	canSampleRate = 10e6 // 10 MS/s — plenty at 500 kbit/s
	canVHigh      = 3300.0
	canVLow       = 0.0
)

func defaultCANConfig() map[string]any {
	return map[string]any{
		"bitRate":     500_000.0,
		"samplePoint": 0.5,
		"thresholdMv": 1650.0,
	}
}

func decodeCAN(t *testing.T, samples []float64, dtNs float64,
	cfg map[string]any) []Event {
	t.Helper()
	events, _, err := DecodeCAN(Context{
		SamplesA: samples, DtNs: dtNs, RangeMvA: 5000,
		Config:     cfg,
		ChannelMap: map[string]string{"CAN": "A"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return events
}

func TestCAN_StandardFrame(t *testing.T) {
	samples, dt := SynthCAN([]CANFrame{{
		ID: 0x123, Data: []byte{0x11, 0x22, 0x33, 0x44},
	}}, 500_000, canSampleRate, canVHigh, canVLow, 500, 500)

	events := decodeCAN(t, samples, dt, defaultCANConfig())
	var got *Event
	for i := range events {
		if events[i].Kind == "frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no frame decoded (events=%+v)", events)
	}
	if got.Value != 0x123 {
		t.Errorf("want ID=0x123, got 0x%X", got.Value)
	}
}

func TestCAN_ExtendedFrame(t *testing.T) {
	samples, dt := SynthCAN([]CANFrame{{
		ID: 0x12345678, Extended: true, Data: []byte{0xAA, 0xBB},
	}}, 500_000, canSampleRate, canVHigh, canVLow, 500, 500)

	events := decodeCAN(t, samples, dt, defaultCANConfig())
	var got *Event
	for i := range events {
		if events[i].Kind == "frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no frame decoded (events=%+v)", events)
	}
	if got.Value != 0x12345678 {
		t.Errorf("want ID=0x12345678, got 0x%X", got.Value)
	}
}

func TestCAN_RTR(t *testing.T) {
	samples, dt := SynthCAN([]CANFrame{{
		ID: 0x7FF, RTR: true,
	}}, 500_000, canSampleRate, canVHigh, canVLow, 500, 500)

	events := decodeCAN(t, samples, dt, defaultCANConfig())
	var got *Event
	for i := range events {
		if events[i].Kind == "frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no RTR frame decoded (events=%+v)", events)
	}
	if got.Value != 0x7FF {
		t.Errorf("want ID=0x7FF, got 0x%X", got.Value)
	}
}

func TestCAN_BackToBackFrames(t *testing.T) {
	samples, dt := SynthCAN([]CANFrame{
		{ID: 0x100, Data: []byte{0x01}},
		{ID: 0x200, Data: []byte{0x02}},
	}, 500_000, canSampleRate, canVHigh, canVLow, 500, 500)

	events := decodeCAN(t, samples, dt, defaultCANConfig())
	var ids []int
	for _, e := range events {
		if e.Kind == "frame" {
			ids = append(ids, e.Value)
		}
	}
	if len(ids) != 2 || ids[0] != 0x100 || ids[1] != 0x200 {
		t.Errorf("want [0x100,0x200], got %v", ids)
	}
}
