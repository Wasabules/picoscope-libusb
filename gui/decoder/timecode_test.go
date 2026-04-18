package decoder

import (
	"testing"
)

const (
	tcSampleRate = 10e3 // 10 kS/s — plenty for DCF (100 ms pulses) and IRIG (2 ms)
	tcVHigh      = 3300.0
	tcVLow       = 0.0
)

func TestClassifier_DCF77(t *testing.T) {
	samples, dt := SynthDCF77(DCF77Frame{
		MinuteUnits: 5, MinuteTens: 4,
		HourUnits: 2, HourTens: 1,
		DayUnits: 1, DayTens: 0,
		MonthUnits: 4, YearUnits: 6, YearTens: 2,
	}, 62, tcSampleRate, tcVHigh, tcVLow)

	fp := ClassifyTimecode(samples, dt)
	if fp.Format != TCDCF77 {
		t.Fatalf("want DCF77, got %s (debug=%s)", fp.Format, fp.Debug)
	}
	if fp.Confidence < 0.8 {
		t.Errorf("want confidence ≥ 0.8, got %.2f", fp.Confidence)
	}
}

func TestClassifier_IRIGB(t *testing.T) {
	samples, dt := SynthIRIGB(42, 30, 15, 183, 26, tcSampleRate, tcVHigh, tcVLow)
	// Give the classifier multiple seconds so PPS estimate stabilises.
	multi := make([]float64, 0, len(samples)*5)
	for k := 0; k < 5; k++ {
		multi = append(multi, samples...)
	}
	fp := ClassifyTimecode(multi, dt)
	if fp.Format != TCIRIGB {
		t.Fatalf("want IRIGB, got %s (debug=%s)", fp.Format, fp.Debug)
	}
	if fp.Confidence < 0.7 {
		t.Errorf("want confidence ≥ 0.7, got %.2f", fp.Confidence)
	}
}

func TestDCF77_Decode(t *testing.T) {
	samples, dt := SynthDCF77(DCF77Frame{
		MinuteUnits: 5, MinuteTens: 4, // 45 min
		HourUnits: 2, HourTens: 1, // 12h (units=2, tens=1) = 12
		DayUnits: 1, DayTens: 0, // 1st
		MonthUnits: 4, // April
		YearUnits:  6, YearTens: 2, // 26
	}, 120, tcSampleRate, tcVHigh, tcVLow)

	events, _, err := DecodeDCF77(Context{
		SamplesA: samples, DtNs: dt, RangeMvA: 5000,
		Config: map[string]any{}, ChannelMap: map[string]string{"DATA": "A"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	var got *Event
	for i := range events {
		if events[i].Kind == "dcf77_frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no dcf77_frame event (events=%+v)", events)
	}
	if got.Annotation != "12:45" {
		t.Errorf("want 12:45 annotation, got %q (text=%q)", got.Annotation, got.Text)
	}
}

func TestIRIGB_Decode(t *testing.T) {
	samples, dt := SynthIRIGB(42, 30, 15, 183, 26, tcSampleRate, tcVHigh, tcVLow)
	// Duplicate for 3 s so we have at least one clean frame.
	multi := make([]float64, 0, len(samples)*3)
	for k := 0; k < 3; k++ {
		multi = append(multi, samples...)
	}
	events, _, err := DecodeIRIG(Context{
		SamplesA: multi, DtNs: dt, RangeMvA: 5000,
		Config: map[string]any{}, ChannelMap: map[string]string{"DATA": "A"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	var got *Event
	for i := range events {
		if events[i].Kind == "irig_frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no irig_frame event (events=%+v)", events)
	}
	if got.Annotation != "15:30:42" {
		t.Errorf("want 15:30:42, got %q (text=%q)", got.Annotation, got.Text)
	}
}

func TestAFNOR_Decode(t *testing.T) {
	samples, dt := SynthAFNOR_S87_500(7, 15, 9, 42, 26, tcSampleRate, tcVHigh, tcVLow)
	multi := make([]float64, 0, len(samples)*3)
	for k := 0; k < 3; k++ {
		multi = append(multi, samples...)
	}
	events, _, err := DecodeAFNOR(Context{
		SamplesA: multi, DtNs: dt, RangeMvA: 5000,
		Config: map[string]any{}, ChannelMap: map[string]string{"DATA": "A"},
	})
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	var got *Event
	for i := range events {
		if events[i].Kind == "afnor_frame" {
			got = &events[i]
			break
		}
	}
	if got == nil {
		t.Fatalf("no afnor_frame event (events=%+v)", events)
	}
	if got.Annotation != "09:15:07" {
		t.Errorf("want 09:15:07, got %q (text=%q)", got.Annotation, got.Text)
	}
}

func TestTimecodeAuto_DispatchesIRIG(t *testing.T) {
	samples, dt := SynthIRIGB(0, 0, 12, 1, 26, tcSampleRate, tcVHigh, tcVLow)
	multi := make([]float64, 0, len(samples)*3)
	for k := 0; k < 3; k++ {
		multi = append(multi, samples...)
	}
	res := Decode("timecode_auto", Context{
		SamplesA: multi, DtNs: dt, RangeMvA: 5000,
		Config: map[string]any{}, ChannelMap: map[string]string{"DATA": "A"},
	})
	if res.Error != "" {
		t.Fatalf("decode error: %s", res.Error)
	}
	var seenClass, seenIrig bool
	for _, e := range res.Events {
		if e.Kind == "classified" {
			seenClass = true
		}
		if e.Kind == "irig_frame" {
			seenIrig = true
		}
	}
	if !seenClass || !seenIrig {
		t.Errorf("want classified+irig_frame events; classified=%v irig=%v events=%+v",
			seenClass, seenIrig, res.Events)
	}
}
