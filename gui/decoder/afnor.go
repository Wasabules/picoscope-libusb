package decoder

import "strings"

// DecodeAFNOR implements the French NF S87-500 time code. The physical
// carrier is identical to IRIG-B DC-shift (100 bits/s, 2/5/8 ms pulses)
// but the bit layout differs: NF S87-500 places seconds in slots 1..8
// like IRIG, but carries additional French administrative fields past
// the year block. For our scope the decoder extracts the core TOY
// (hours/minutes/seconds/day-of-year/year) exactly like IRIG-B and tags
// the output as AFNOR so the user can tell the formats apart in the log.
func DecodeAFNOR(ctx Context) ([]Event, []string, error) {
	events, dbg, err := DecodeIRIG(ctx)
	if err != nil {
		return events, dbg, err
	}
	for i := range events {
		if events[i].Kind != "irig_frame" {
			continue
		}
		events[i].Kind = "afnor_frame"
		// Replace the "IRIG-Bxxx" prefix with "AFNOR" for display.
		if idx := strings.Index(events[i].Text, " "); idx > 0 {
			events[i].Text = "AFNOR" + events[i].Text[idx:]
		}
	}
	dbg = append(dbg, "afnor: re-tagged IRIG-style frames as NF S87-500")
	return events, dbg, nil
}
