package main

import (
	"encoding/base64"
	"fmt"
	"os"
	"strings"
	"time"

	wailsRuntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

// ExportCSV writes the supplied samples to a CSV file chosen by the user
// through a native save dialog. `dtNs` is the per-sample time spacing;
// `chB` may be nil for CH A only. Returns the full path on success.
func (a *App) ExportCSV(chA []float64, chB []float64, dtNs float64) (string, error) {
	path, err := wailsRuntime.SaveFileDialog(a.ctx, wailsRuntime.SaveDialogOptions{
		Title:           "Export waveform",
		DefaultFilename: fmt.Sprintf("picoscope_%d.csv", time.Now().Unix()),
		Filters: []wailsRuntime.FileFilter{
			{DisplayName: "CSV (*.csv)", Pattern: "*.csv"},
		},
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil // user cancelled
	}
	if !strings.HasSuffix(strings.ToLower(path), ".csv") {
		path += ".csv"
	}
	f, err := os.Create(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	if len(chB) > 0 {
		fmt.Fprintln(f, "t_ns,ch_a_mv,ch_b_mv")
	} else {
		fmt.Fprintln(f, "t_ns,ch_a_mv")
	}
	n := len(chA)
	if len(chB) > 0 && len(chB) < n {
		n = len(chB)
	}
	buf := &strings.Builder{}
	buf.Grow(n * 32)
	for i := 0; i < n; i++ {
		t := float64(i) * dtNs
		if len(chB) > 0 {
			fmt.Fprintf(buf, "%.1f,%.3f,%.3f\n", t, chA[i], chB[i])
		} else {
			fmt.Fprintf(buf, "%.1f,%.3f\n", t, chA[i])
		}
	}
	if _, err := f.WriteString(buf.String()); err != nil {
		return "", err
	}
	return path, nil
}

// ExportPNG writes a canvas screenshot (supplied as a base64-encoded
// `data:image/png;base64,...` URL from the frontend) to disk.
func (a *App) ExportPNG(dataUrl string) (string, error) {
	// Accept either "data:image/png;base64,XXX" or raw base64.
	const prefix = "base64,"
	idx := strings.Index(dataUrl, prefix)
	if idx >= 0 {
		dataUrl = dataUrl[idx+len(prefix):]
	}
	img, err := base64.StdEncoding.DecodeString(dataUrl)
	if err != nil {
		return "", fmt.Errorf("bad base64 image: %w", err)
	}
	path, err := wailsRuntime.SaveFileDialog(a.ctx, wailsRuntime.SaveDialogOptions{
		Title:           "Save screenshot",
		DefaultFilename: fmt.Sprintf("picoscope_%d.png", time.Now().Unix()),
		Filters: []wailsRuntime.FileFilter{
			{DisplayName: "PNG (*.png)", Pattern: "*.png"},
		},
	})
	if err != nil {
		return "", err
	}
	if path == "" {
		return "", nil
	}
	if !strings.HasSuffix(strings.ToLower(path), ".png") {
		path += ".png"
	}
	if err := os.WriteFile(path, img, 0644); err != nil {
		return "", err
	}
	return path, nil
}
