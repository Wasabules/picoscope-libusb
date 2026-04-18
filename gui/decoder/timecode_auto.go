package decoder

import "fmt"

// decodeTimecodeAuto runs the classifier and dispatches to the matching
// decoder. The classifier's fingerprint is prepended as a synthetic
// "classified" event so the UI can display the detected format without a
// separate API.
func decodeTimecodeAuto(ctx Context) ([]Event, []string, error) {
	chKey := "A"
	if m, ok := ctx.ChannelMap["DATA"]; ok && (m == "A" || m == "B") {
		chKey = m
	}
	samples := ctx.SamplesA
	if chKey == "B" {
		samples = ctx.SamplesB
	}
	fp := ClassifyTimecode(samples, ctx.DtNs)
	classified := Event{
		Kind: "classified", Level: "info",
		Annotation: string(fp.Format),
		Text: fmt.Sprintf("auto-classifier: format=%s confidence=%.2f pps=%.2f median=%.1fms (%s)",
			fp.Format, fp.Confidence, fp.PulsesPerS, fp.MedianMs, fp.Debug),
	}

	var events []Event
	var dbg []string
	var err error
	switch fp.Format {
	case TCDCF77:
		events, dbg, err = DecodeDCF77(ctx)
	case TCIRIGB:
		events, dbg, err = DecodeIRIG(ctx)
	case TCAFNOR:
		events, dbg, err = DecodeAFNOR(ctx)
	default:
		events = []Event{{
			Kind: "warn", Level: "warn", Annotation: "NO MATCH",
			Text: classified.Text,
		}}
	}
	events = append([]Event{classified}, events...)
	dbg = append([]string{classified.Text}, dbg...)
	return events, dbg, err
}
