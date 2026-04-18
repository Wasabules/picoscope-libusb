package main

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	wailsRuntime "github.com/wailsapp/wails/v2/pkg/runtime"
)

// FirmwareStatusInfo mirrors the driver's expected firmware layout and what
// helper tools are available on this host.
type FirmwareStatusInfo struct {
	Installed         bool           `json:"installed"`
	Dir               string         `json:"dir"`
	Files             map[string]int `json:"files"` // name → size; missing keys = absent
	SdkAvailable      bool           `json:"sdkAvailable"`
	SdkLibPath        string         `json:"sdkLibPath"`
	InterceptorPath   string         `json:"interceptorPath"`
	PcapToolPath      string         `json:"pcapToolPath"`
	TsharkAvailable   bool           `json:"tsharkAvailable"`
}

// firmwareFiles is the canonical set the driver expects in the firmware dir.
var firmwareFiles = []string{"fx2.bin", "fpga.bin", "waveform.bin", "stream_lut.bin"}

// firmwareDir resolves the search path the C driver uses.
// Order: $PS2204A_FIRMWARE_DIR → $XDG_CONFIG_HOME/picoscope-libusb/firmware
//        → $HOME/.config/picoscope-libusb/firmware.
func firmwareDir() string {
	if v := os.Getenv("PS2204A_FIRMWARE_DIR"); v != "" {
		return v
	}
	if v := os.Getenv("XDG_CONFIG_HOME"); v != "" {
		return filepath.Join(v, "picoscope-libusb", "firmware")
	}
	if v := os.Getenv("HOME"); v != "" {
		return filepath.Join(v, ".config", "picoscope-libusb", "firmware")
	}
	return ""
}

// walkUpFor searches `start` and all its ancestors for `rel`. Returns the
// absolute path to the first match, or "" if none found.
func walkUpFor(start, rel string) string {
	cur := start
	for i := 0; i < 8; i++ {
		p := filepath.Join(cur, rel)
		if _, err := os.Stat(p); err == nil {
			abs, _ := filepath.Abs(p)
			return abs
		}
		parent := filepath.Dir(cur)
		if parent == cur {
			break
		}
		cur = parent
	}
	return ""
}

// findExtractorFile locates one of the extractor tools. Order:
// 1. alongside the executable (production install)
// 2. walk up from the executable looking for tools/firmware-extractor/<name>
// 3. walk up from CWD for the same
// 4. system dirs
func findExtractorFile(name string, sysDirs []string) string {
	rel := filepath.Join("tools", "firmware-extractor", name)
	if exe, err := os.Executable(); err == nil {
		dir := filepath.Dir(exe)
		if p := filepath.Join(dir, name); fileExists(p) {
			abs, _ := filepath.Abs(p)
			return abs
		}
		if p := walkUpFor(dir, rel); p != "" {
			return p
		}
	}
	if cwd, err := os.Getwd(); err == nil {
		if p := walkUpFor(cwd, rel); p != "" {
			return p
		}
	}
	for _, d := range sysDirs {
		p := filepath.Join(d, name)
		if fileExists(p) {
			return p
		}
	}
	return ""
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func findInterceptor() string {
	if v := os.Getenv("PS_INTERCEPT_SO"); v != "" {
		if fileExists(v) {
			return v
		}
	}
	return findExtractorFile("libps_intercept.so", []string{
		"/usr/local/lib/picoscope-libusb",
		"/usr/lib/picoscope-libusb",
	})
}

func findPcapTool() string {
	return findExtractorFile("extract-from-pcap.py", []string{
		"/usr/local/lib/picoscope-libusb",
		"/usr/lib/picoscope-libusb",
	})
}

func findSdkLib() string {
	for _, p := range []string{
		"/opt/picoscope/lib/libps2000.so",
		"/usr/lib/libps2000.so",
		"/usr/lib/x86_64-linux-gnu/libps2000.so",
	} {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

func haveTshark() bool {
	_, err := exec.LookPath("tshark")
	return err == nil
}

// FirmwareStatus reports whether the driver will find usable firmware and
// which extraction paths are viable on this host. Cheap to call — the
// frontend runs it on startup and again after every extraction attempt.
func (a *App) FirmwareStatus() FirmwareStatusInfo {
	dir := firmwareDir()
	info := FirmwareStatusInfo{
		Dir:             dir,
		Files:           map[string]int{},
		SdkLibPath:      findSdkLib(),
		InterceptorPath: findInterceptor(),
		PcapToolPath:    findPcapTool(),
		TsharkAvailable: haveTshark(),
	}
	info.SdkAvailable = info.SdkLibPath != "" && info.InterceptorPath != ""

	allPresent := dir != ""
	for _, name := range firmwareFiles {
		if dir == "" {
			allPresent = false
			break
		}
		p := filepath.Join(dir, name)
		if st, err := os.Stat(p); err == nil && st.Size() > 0 {
			info.Files[name] = int(st.Size())
		} else {
			allPresent = false
		}
	}
	info.Installed = allPresent
	return info
}

// emitFw pushes a progress line to the frontend. The UI tails them in a log
// view so the user can see what the extractor is doing.
func (a *App) emitFw(line string) {
	if a.ctx != nil {
		wailsRuntime.EventsEmit(a.ctx, "fwExtractLog", line)
	}
}

// ExtractFirmwareLive drives the SDK-open path: spawns a tiny Python helper
// that calls ps2000_open_unit()/close, under LD_PRELOAD=libps_intercept.so,
// which captures the firmware blobs as they go over USB.
//
// Precondition: the scope must be freshly plugged in. If firmware was
// already uploaded this session (by another program or our own driver),
// the SDK won't re-upload it and the extraction comes up empty.
func (a *App) ExtractFirmwareLive() error {
	info := a.FirmwareStatus()
	if !info.SdkAvailable {
		return fmt.Errorf("live extraction needs the official Pico SDK at /opt/picoscope/ (libps2000.so) and libps_intercept.so; install the Pico suite and rebuild tools/firmware-extractor")
	}
	if a.connected {
		return fmt.Errorf("disconnect the scope from the app first (the driver holds the USB interface)")
	}

	// Spawn a short-lived python that opens the scope via the SDK. This is
	// what triggers the firmware upload we intercept.
	pyScript := fmt.Sprintf(`
import ctypes, time, sys
L = ctypes.CDLL(%q)
h = L.ps2000_open_unit()
if h <= 0:
    sys.stderr.write("ps2000_open_unit() returned %%d — replug the scope and retry\n" %% h)
    sys.exit(2)
time.sleep(0.5)
L.ps2000_close_unit(ctypes.c_int16(h))
print("ok")
`, info.SdkLibPath)

	a.emitFw("[fw] launching SDK helper under LD_PRELOAD…")
	a.emitFw("[fw] interceptor: " + info.InterceptorPath)
	a.emitFw("[fw] SDK lib:     " + info.SdkLibPath)

	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, "python3", "-c", pyScript)

	// Inherit env; prepend LD_PRELOAD + LD_LIBRARY_PATH. The interceptor writes
	// the blobs to firmwareDir() itself, so no output parsing required.
	env := os.Environ()
	env = append(env,
		"LD_PRELOAD="+info.InterceptorPath,
		"LD_LIBRARY_PATH="+filepath.Dir(info.SdkLibPath),
	)
	if info.Dir != "" {
		env = append(env, "PS2204A_FIRMWARE_DIR="+info.Dir)
	}
	cmd.Env = env

	// Stream combined stdout+stderr to the UI log.
	out, err := cmd.CombinedOutput()
	for _, ln := range strings.Split(strings.TrimRight(string(out), "\n"), "\n") {
		if ln != "" {
			a.emitFw(ln)
		}
	}
	if err != nil {
		return fmt.Errorf("extraction helper failed: %v", err)
	}

	after := a.FirmwareStatus()
	if !after.Installed {
		missing := []string{}
		for _, f := range firmwareFiles {
			if _, ok := after.Files[f]; !ok {
				missing = append(missing, f)
			}
		}
		return fmt.Errorf("extraction ran but firmware is incomplete (missing: %s). Unplug/replug the scope and retry — the SDK only re-uploads firmware on a fresh power-up",
			strings.Join(missing, ", "))
	}
	a.emitFw("[fw] extraction OK — " + fmt.Sprintf("%d files in %s", len(after.Files), after.Dir))
	return nil
}

// ExtractFirmwareFromPcap runs the offline pcap extractor. Used when the
// user doesn't have the official SDK installed but has a capture from
// someone who does.
func (a *App) ExtractFirmwareFromPcap(pcapPath string) error {
	info := a.FirmwareStatus()
	if info.PcapToolPath == "" {
		return fmt.Errorf("pcap extractor not found (expected extract-from-pcap.py alongside the app or in tools/firmware-extractor/)")
	}
	if !info.TsharkAvailable {
		return fmt.Errorf("tshark not installed — apt install tshark (required for pcap parsing)")
	}
	if pcapPath == "" {
		return fmt.Errorf("no pcap file selected")
	}
	if _, err := os.Stat(pcapPath); err != nil {
		return fmt.Errorf("pcap file not readable: %v", err)
	}

	a.emitFw("[fw] running extract-from-pcap.py on " + pcapPath)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Minute)
	defer cancel()

	args := []string{info.PcapToolPath, pcapPath}
	if info.Dir != "" {
		args = append(args, "--out", info.Dir)
	}
	cmd := exec.CommandContext(ctx, "python3", args...)

	out, err := cmd.CombinedOutput()
	for _, ln := range strings.Split(strings.TrimRight(string(out), "\n"), "\n") {
		if ln != "" {
			a.emitFw(ln)
		}
	}
	if err != nil {
		return fmt.Errorf("pcap extractor failed: %v", err)
	}

	after := a.FirmwareStatus()
	if !after.Installed {
		return fmt.Errorf("pcap did not contain a complete firmware upload (%d/%d files extracted) — record a fresh capture covering the scope's first open",
			len(after.Files), len(firmwareFiles))
	}
	a.emitFw("[fw] extraction OK")
	return nil
}

// PickPcapFile opens a native file dialog and returns the chosen path.
// Empty string means the user cancelled.
func (a *App) PickPcapFile() (string, error) {
	if a.ctx == nil {
		return "", fmt.Errorf("no UI context")
	}
	return wailsRuntime.OpenFileDialog(a.ctx, wailsRuntime.OpenDialogOptions{
		Title: "Select a usbmon pcap capture",
		Filters: []wailsRuntime.FileFilter{
			{DisplayName: "pcap files", Pattern: "*.pcap;*.pcapng"},
			{DisplayName: "all files", Pattern: "*.*"},
		},
	})
}
