package main

import "gui/decoder"

// ListDecoders returns every decoder the frontend can drive.
func (a *App) ListDecoders() []decoder.Descriptor {
	return decoder.List()
}

// StartDecoder spins up a stateful streaming decoder. Subsequent waveform
// blocks fed by the streaming goroutine will be decoded and emitted on
// the 'decoderEvents' Wails event. Calling it a second time replaces the
// active session (configuration change).
func (a *App) StartDecoder(protocol string, config map[string]any,
	channelMap map[string]string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.decSession != nil {
		a.decSession.Close()
	}
	a.decSession = decoder.NewSession(protocol, config, channelMap)
	return nil
}

// StopDecoder tears down the streaming decoder. Safe to call when no
// session is active.
func (a *App) StopDecoder() {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.decSession != nil {
		a.decSession.Close()
		a.decSession = nil
	}
}

// Decode runs one protocol-decoder pass and returns the Result. Any error
// (invalid protocol, bad dt_ns, etc.) is surfaced via Result.Error rather
// than a Go error — easier for the JS side to render uniformly.
func (a *App) Decode(req DecodeRequest) decoder.Result {
	ctx := decoder.Context{
		SamplesA:   req.SamplesA,
		SamplesB:   req.SamplesB,
		DtNs:       req.DtNs,
		RangeMvA:   req.RangeMvA,
		RangeMvB:   req.RangeMvB,
		Config:     req.Config,
		ChannelMap: req.ChannelMap,
	}
	return decoder.Decode(req.Protocol, ctx)
}
