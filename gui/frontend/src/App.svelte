<script>
  import { onMount, onDestroy } from 'svelte';
  import { fmtMv, fmtHz, fmtRate, fmtCount, fmtTime, fmtMeas } from './lib/utils/format.js';
  import { RING_CAP, createRing, pushRing, materializeRing } from './lib/dsp/ring.js';
  import { fftMag } from './lib/dsp/fft.js';
  import { computeMath } from './lib/dsp/math.js';
  import { computeMeas } from './lib/dsp/measurements.js';
  import { makeEmptyStats, pushStat, aggregateStatsBag } from './lib/dsp/stats.js';
  import WaveformCanvas from './lib/components/WaveformCanvas.svelte';
  import DecoderPanel from './lib/components/DecoderPanel.svelte';
  import DecoderLog from './lib/components/DecoderLog.svelte';
  import ErrorToast from './lib/components/ErrorToast.svelte';
  import StatusBar from './lib/components/StatusBar.svelte';
  import MeasurementsBar from './lib/components/MeasurementsBar.svelte';
  import ChannelPanel from './lib/components/ChannelPanel.svelte';
  import TimebaseControls from './lib/components/TimebaseControls.svelte';
  import TriggerControls from './lib/components/TriggerControls.svelte';
  import SiggenControls from './lib/components/SiggenControls.svelte';
  import CalibrationPanel from './lib/components/CalibrationPanel.svelte';
  import PresetsPanel from './lib/components/PresetsPanel.svelte';
  import DiagnosticsPanel from './lib/components/DiagnosticsPanel.svelte';
  import DisplayControls from './lib/components/DisplayControls.svelte';
  import AnalysisControls from './lib/components/AnalysisControls.svelte';
  import CalibrationModal from './lib/modals/CalibrationModal.svelte';
  import FirmwareSetupModal from './lib/modals/FirmwareSetupModal.svelte';
  import { EventsOn, EventsOff } from '../wailsjs/runtime/runtime.js';
  import {
    Connect, Disconnect, IsConnected, GetDeviceInfo,
    SetChannelA, SetChannelB, GetChannelA, GetChannelB,
    SetTimebase, GetTimebase, GetAllTimebases, GetTimebaseNs,
    SetSamples, GetSamples, GetMaxSamples,
    CaptureBlock, CaptureRaw,
    StartStreaming, StartStreamingMode, StopStreaming, IsStreaming, GetStreamStats,
    SetTrigger, DisableTrigger,
    SetSiggen, SetSiggenEx, DisableSiggen,
    CalibrateDCOffset, SetRangeCalibration,
    ComputeMeasurements,
    ExportCSV, ExportPNG,
    FirmwareStatus
  } from '../wailsjs/go/main/App.js';

  // Connection state
  let connected = false;
  let serial = '';
  let calDate = '';
  let connecting = false;

  // Channel config
  let chAEnabled = true;
  let chACoupling = 'DC';
  let chARange = '5V';
  let chBEnabled = false;
  let chBCoupling = 'DC';
  let chBRange = '5V';

  // Timebase & sampling
  let timebase = 5;
  let timebases = [];
  let samples = 8064;
  let maxSamples = 8064;

  // Streaming mode
  let streamMode = 'fast'; // 'fast' or 'native'
  let rawPreview = null;   // last raw capture

  // Trigger
  let triggerEnabled = false;
  let triggerSource = 'A';
  let triggerThreshold = 0;
  let triggerDirection = 'rising';
  let triggerAutoMs = 5000;

  // Signal generator (fixed freq + extended sweep/offset/duty options)
  let siggenEnabled = false;
  let siggenWave = 'sine';
  let siggenFreq = 1000;
  let siggenAmpMv = 1000;   // peak-to-peak amplitude (mV)
  let siggenOffsetMv = 0;   // DC offset (mV)
  let siggenDuty = 50;      // square wave duty cycle (%)
  let siggenSweep = false;
  let siggenStopFreq = 5000;
  let siggenSweepInc = 100;
  let siggenSweepDwell = 0.05;

  // Waveform data (last single-shot capture)
  let waveformData = null;
  let isStreaming = false;
  let capturing = false;

  // Streaming stats
  let streamStats = null;

  // Measurements (computed on visible slice)
  let measA = null;
  let measB = null;
  let measM = null; // math channel measurements

  // User-selectable measurement panel. Every metric in `MEAS_CATALOG` is
  // always computed (the cost is negligible on a 1-div slice), but the
  // bottom bar only renders the keys in `measKeys`. Grouped Amplitude / Time
  // so the picker stays readable as we add more metrics.
  const MEAS_CATALOG = [
    { key: 'vpp',    label: 'Vpp',    group: 'amp',  fmt: 'mv'   },
    { key: 'min',    label: 'Vmin',   group: 'amp',  fmt: 'mv'   },
    { key: 'max',    label: 'Vmax',   group: 'amp',  fmt: 'mv'   },
    { key: 'mean',   label: 'Mean',   group: 'amp',  fmt: 'mv'   },
    { key: 'rms',    label: 'RMS',    group: 'amp',  fmt: 'mv'   },
    { key: 'freqHz', label: 'Freq',   group: 'time', fmt: 'hz'   },
    { key: 'periodNs',label:'Period', group: 'time', fmt: 'time' },
    { key: 'duty',   label: 'Duty',   group: 'time', fmt: 'pct'  },
    { key: 'riseNs', label: 'Rise',   group: 'time', fmt: 'time' },
    { key: 'fallNs', label: 'Fall',   group: 'time', fmt: 'time' },
  ];
  const MEAS_DEFAULT = ['vpp', 'mean', 'rms', 'freqHz', 'duty'];
  let measKeys = new Set(MEAS_DEFAULT);
  let measMenuOpen = false;
  function toggleMeasKey(key) {
    if (measKeys.has(key)) measKeys.delete(key);
    else measKeys.add(key);
    measKeys = new Set(measKeys); // trigger reactivity
  }
  // Math channel + cursors + XY mode
  let mathOp = 'none';       // none | add | sub | mul | inva | invb
  let cursorsOn = false;
  let cursor1Pct = 30;       // 0..100 horizontal position
  let cursor2Pct = 70;
  let yCursorsOn = false;
  let yCursor1Mv = 200;
  let yCursor2Mv = -200;
  let xyMode = false;
  // Per-channel vertical offset (mV) + V/div (0 = auto from range)
  let offsetMvA = 0;
  let offsetMvB = 0;
  let vdivMvA = 0;
  let vdivMvB = 0;
  // FFT + averaging toggles (Phase 2)
  let fftOn = false;
  let avgN = 1;              // 1 | 4 | 16 | 64
  let persistenceOn = false;

  // --- Display window / scope controls ---
  // timePerDivNs = 0 -> auto (fit entire buffer). Otherwise total visible = timePerDivNs * 10.
  const TIME_DIV_PRESETS = [
    { ns: 0,     label: 'Auto' },
    { ns: 1e3,   label: '1 µs'   },
    { ns: 2e3,   label: '2 µs'   },
    { ns: 5e3,   label: '5 µs'   },
    { ns: 1e4,   label: '10 µs'  },
    { ns: 2e4,   label: '20 µs'  },
    { ns: 5e4,   label: '50 µs'  },
    { ns: 1e5,   label: '100 µs' },
    { ns: 2e5,   label: '200 µs' },
    { ns: 5e5,   label: '500 µs' },
    { ns: 1e6,   label: '1 ms'   },
    { ns: 2e6,   label: '2 ms'   },
    { ns: 5e6,   label: '5 ms'   },
    { ns: 1e7,   label: '10 ms'  },
    { ns: 2e7,   label: '20 ms'  },
    { ns: 5e7,   label: '50 ms'  },
    { ns: 1e8,   label: '100 ms' },
    { ns: 2e8,   label: '200 ms' },
    { ns: 5e8,   label: '500 ms' },
    { ns: 1e9,   label: '1 s'    },
    { ns: 2e9,   label: '2 s'    },
    { ns: 5e9,   label: '5 s'    },
    { ns: 1e10,  label: '10 s'   },
  ];
  let timePerDivNs = 0;        // 0 = auto
  let displayOffsetPct = 100;  // 0..100, where the trailing edge sits in the buffer
  let streamPaused = false;

  // Frame averaging: keep the last N blocks for each channel and show the
  // pointwise mean. Resets when the user changes avgN or channel enables.
  let avgQueueA = [];
  let avgQueueB = [];
  function resetAvg() { avgQueueA = []; avgQueueB = []; }
  $: if (avgN || chAEnabled || chBEnabled) resetAvg();

  function pushAvg(queue, arr) {
    if (!arr || !arr.length) return queue;
    // Queue may hold arrays of slightly different lengths; we clip to the
    // smallest common length before summing.
    queue.push(arr);
    while (queue.length > avgN) queue.shift();
    return queue;
  }
  function avgBlocks(queue) {
    if (queue.length === 0) return null;
    if (queue.length === 1) return queue[0];
    let n = queue[0].length;
    for (let i = 1; i < queue.length; i++) n = Math.min(n, queue[i].length);
    const out = new Float64Array(n);
    for (let i = 0; i < n; i++) {
      let s = 0;
      for (let k = 0; k < queue.length; k++) s += queue[k][i];
      out[i] = s / queue.length;
    }
    return out;
  }

  // Streaming JS-side ring buffers (kept independent from the C ring buffer).
  // Circular buffer with a fixed pre-allocation so large rings don't hammer GC
  // (re-allocating a 1 M Float32Array at 55 Hz = ~220 MB/s churn, which froze
  // the canvas in the previous implementation).
  // 4M samples ≈ 5.37 s at fast-streaming dt=1280 ns, enough to cover up to
  // 500 ms/div × 10 divisions. At 2 channels × 4 bytes that's 32 MB — fine.
  let streamRingA = null;   // Float32Array(RING_CAP) — allocated on first use
  let streamRingB = null;
  let streamRingALen  = 0;  // valid sample count (≤ RING_CAP)
  let streamRingBLen  = 0;
  let streamRingHeadA = 0;  // next write index into the circular buffer
  let streamRingHeadB = 0;
  // Per-sample wall-clock spacing (ns) — updated from streamStats.samplesPerSec.
  let effectiveDtNs = 0;
  // Last-known per-sample metadata while streaming
  let lastStreamTimebaseNs = 0;
  let lastStreamRangeMvA = 5000;
  let lastStreamRangeMvB = 5000;

  // Error handling
  let errorMsg = '';
  let errorTimeout;

  const RANGES = ['50mV', '100mV', '200mV', '500mV', '1V', '2V', '5V', '10V', '20V'];
  // Match driver RANGE_MV[]: 5V / 10V calibrated; 50mV uses digital scaling.
  const RANGE_MV = {
    '50mV': 50, '100mV': 100, '200mV': 200, '500mV': 500,
    '1V': 1000, '2V': 2000, '5V': 3515, '10V': 9092, '20V': 20000
  };
  const WAVE_TYPES = ['sine', 'square', 'triangle', 'rampup', 'rampdown', 'dc'];

  function showError(msg) {
    errorMsg = msg;
    clearTimeout(errorTimeout);
    errorTimeout = setTimeout(() => { errorMsg = ''; }, 4000);
  }

  // Connection
  async function handleConnect() {
    if (connected) {
      try {
        if (isStreaming) await handleStopStreaming();
        await Disconnect();
        connected = false;
        serial = '';
        calDate = '';
        waveformData = null;
        streamStats = null;
        clearRings();
        effectiveDtNs = 0;
      } catch (e) { showError(e); }
    } else {
      connecting = true;
      try {
        const info = await Connect();
        connected = info.connected;
        serial = info.serial;
        calDate = info.calDate;
        timebases = await GetAllTimebases();
        await refreshSampleLimits();
      } catch (e) {
        showError(String(e));
      }
      connecting = false;
    }
  }

  // Channel changes. Fast streaming pre-builds cmd1 at thread start with
  // the PGA gain bytes baked in, so a mid-stream range change updates only
  // the cached state — the hardware keeps the old gain and the C-side
  // scaling stays unchanged. Restart the stream so the new gain bytes go
  // out on the wire.
  async function updateChannelA() {
    if (!connected) return;
    try {
      await SetChannelA(chAEnabled, chACoupling, chARange);
      await refreshSampleLimits();
      if (isStreaming) await restartStreamForConfigChange();
    } catch (e) { showError(String(e)); }
  }

  async function updateChannelB() {
    if (!connected) return;
    try {
      await SetChannelB(chBEnabled, chBCoupling, chBRange);
      await refreshSampleLimits();
      if (isStreaming) await restartStreamForConfigChange();
    } catch (e) { showError(String(e)); }
  }

  async function restartStreamForConfigChange() {
    try {
      await StopStreaming();
      isStreaming = false;
      clearRings();
      effectiveDtNs = 0;
      lastStreamRangeMvA = currentRangeMvA;
      lastStreamRangeMvB = currentRangeMvB;
      await StartStreamingMode(streamMode);
      isStreaming = true;
    } catch (e) { showError(String(e)); }
  }

  // Timebase. Same pre-built-cmd1 caveat as range/coupling: mid-stream
  // changes don't propagate until the stream is restarted.
  async function updateTimebase() {
    if (!connected) return;
    try {
      await SetTimebase(timebase);
      if (isStreaming) await restartStreamForConfigChange();
    } catch (e) { showError(String(e)); }
  }

  async function updateSamples() {
    if (!connected) return;
    try {
      await SetSamples(samples);
      samples = await GetSamples();
    } catch (e) { showError(String(e)); }
  }

  async function refreshSampleLimits() {
    if (!connected) return;
    try {
      maxSamples = await GetMaxSamples();
      samples = await GetSamples();
    } catch (e) { /* ignore */ }
  }

  // Trigger
  async function updateTrigger() {
    if (!connected) return;
    try {
      if (triggerEnabled) {
        await SetTrigger(triggerSource, triggerThreshold, triggerDirection, 0, triggerAutoMs);
      } else {
        await DisableTrigger();
      }
    } catch (e) { showError(String(e)); }
  }

  // Capture
  async function handleCapture() {
    if (!connected || capturing) return;
    capturing = true;
    try {
      const data = await CaptureBlock();
      // Averaging: accumulate last N single-shot captures and display their
      // pointwise mean. Dramatically reduces 8-bit ADC / DAC quantization
      // noise for repetitive signals.
      if (avgN > 1) {
        if (data.channelA) avgQueueA = pushAvg(avgQueueA, data.channelA);
        if (data.channelB) avgQueueB = pushAvg(avgQueueB, data.channelB);
        const avgA = avgBlocks(avgQueueA);
        const avgB = avgBlocks(avgQueueB);
        waveformData = {
          ...data,
          channelA: avgA || data.channelA,
          channelB: avgB || data.channelB,
        };
      } else {
        waveformData = data;
      }
      // Reset single-shot view to beginning when manual zoom is active.
      if (timePerDivNs > 0) displayOffsetPct = 0;
    } catch (e) { showError(String(e)); }
    capturing = false;
  }

  // Streaming
  async function handleStartStreaming() {
    if (!connected || isStreaming) return;
    try {
      clearRings();
      effectiveDtNs = 0;
      streamPaused = false;
      displayOffsetPct = 100;
      lastStreamRangeMvA = currentRangeMvA;
      lastStreamRangeMvB = currentRangeMvB;
      // Pick a sensible rolling window so the view scrolls properly
      // instead of stretching the whole ring. Auto (0) compresses the
      // signal as the ring grows, and a µs-scale value left over from a
      // single-shot session shows 0 samples at native's 100 S/s rate.
      timePerDivNs = streamMode === 'native' ? 1e9 : 1e5;
      await StartStreamingMode(streamMode);
      isStreaming = true;
    } catch (e) { showError(String(e)); }
  }

  async function handleStopStreaming() {
    try {
      await StopStreaming();
      isStreaming = false;
      streamStats = null;
    } catch (e) { showError(String(e)); }
  }

  // Raw diagnostic capture
  async function handleCaptureRaw() {
    if (!connected) return;
    try {
      rawPreview = await CaptureRaw();
    } catch (e) { showError(String(e)); }
  }

  // Signal generator — the 8192-byte siggen LUT goes out on EP 0x06, which
  // the streaming pipeline also uses. Always stop → apply → restart; the
  // SDK setup in the driver picks up the configured siggen state and
  // re-programs the DAC after its own stream_lut prime, so siggen + SDK
  // 1 MS/s streaming coexist. In FAST mode the restart doesn't touch
  // EP 0x06, so the siggen LUT from the apply survives as-is.
  async function withStreamingPaused(fn) {
    const wasStreaming = isStreaming;
    if (wasStreaming) {
      await StopStreaming();
      isStreaming = false;
    }
    try {
      await fn();
    } finally {
      if (wasStreaming) {
        try {
          await StartStreamingMode(streamMode);
          isStreaming = true;
        } catch (e) { showError('Stream restart failed: ' + e); }
      }
    }
  }

  async function handleSiggenOn() {
    if (!connected) return;
    try {
      await withStreamingPaused(async () => {
        const usingExtras = siggenSweep || siggenOffsetMv !== 0
                            || (siggenWave === 'square' && siggenDuty !== 50);
        if (usingExtras) {
          const start = siggenFreq;
          const stop  = siggenSweep ? siggenStopFreq : siggenFreq;
          const inc   = siggenSweep ? siggenSweepInc : 0;
          const dwell = siggenSweep ? siggenSweepDwell : 0;
          await SetSiggenEx(siggenWave, start, stop, inc, dwell,
                            siggenAmpMv, siggenOffsetMv, siggenDuty);
        } else {
          await SetSiggen(siggenWave, siggenFreq, siggenAmpMv);
        }
        siggenEnabled = true;
      });
    } catch (e) { showError(String(e)); }
  }

  async function handleSiggenOff() {
    if (!connected) return;
    try {
      await withStreamingPaused(async () => {
        await DisableSiggen();
        siggenEnabled = false;
      });
    } catch (e) { showError(String(e)); }
  }

  /* ------------ Calibration handlers ------------ */
  let calModalOpen = false;
  let fwModalOpen = false;
  let fwInstalled = true; // optimistic — flipped by the startup probe
  let decoderAnnotations = [];
  let decoderState = { decoder: null, events: [], error: '' };
  let decoderLogCollapsed = false;
  let calRange = '2V';
  let calOffsetMv = 0;
  let calGain = 1.0;
  async function handleCalibrateDCOffset() {
    try {
      await CalibrateDCOffset();
      showError('DC offset calibrated (assumes inputs at 0 V)');
    } catch (e) { showError('Calibration failed: ' + e); }
  }
  async function handleSetRangeCal() {
    try {
      await SetRangeCalibration(calRange, calOffsetMv, calGain);
      showError('Calibration applied to ' + calRange);
    } catch (e) { showError('Failed: ' + e); }
  }

  /* ------------ Presets (settings snapshots in localStorage) ------------ */
  const PRESET_KEY = 'picoscope_presets_v1';
  let presets = {};
  let presetName = '';
  let selectedPreset = '';

  function loadPresetsFromStorage() {
    try {
      const raw = localStorage.getItem(PRESET_KEY);
      presets = raw ? JSON.parse(raw) : {};
    } catch (_) { presets = {}; }
  }
  function savePresetsToStorage() {
    localStorage.setItem(PRESET_KEY, JSON.stringify(presets));
  }
  function snapshotSettings() {
    return {
      chA: { enabled: chAEnabled, coupling: chACoupling, range: chARange,
             offsetMv: offsetMvA, vdivMv: vdivMvA },
      chB: { enabled: chBEnabled, coupling: chBCoupling, range: chBRange,
             offsetMv: offsetMvB, vdivMv: vdivMvB },
      timebase, samples,
      trigger: { enabled: triggerEnabled, source: triggerSource,
                 threshold: triggerThreshold, direction: triggerDirection,
                 autoMs: triggerAutoMs },
      siggen: { wave: siggenWave, freq: siggenFreq, amp: siggenAmpMv,
                enabled: siggenEnabled },
      display: { timePerDivNs, mathOp, xyMode, persistenceOn, avgN, fftOn,
                 cursorsOn, yCursorsOn, measKeys: Array.from(measKeys) },
      streamMode,
    };
  }
  function applySettings(s) {
    if (!s) return;
    if (s.chA) {
      chAEnabled = !!s.chA.enabled; chACoupling = s.chA.coupling || 'DC';
      chARange = s.chA.range || '5V';
      offsetMvA = s.chA.offsetMv || 0; vdivMvA = s.chA.vdivMv || 0;
    }
    if (s.chB) {
      chBEnabled = !!s.chB.enabled; chBCoupling = s.chB.coupling || 'DC';
      chBRange = s.chB.range || '5V';
      offsetMvB = s.chB.offsetMv || 0; vdivMvB = s.chB.vdivMv || 0;
    }
    if (typeof s.timebase === 'number') timebase = s.timebase;
    if (typeof s.samples === 'number') samples = s.samples;
    if (s.trigger) {
      triggerEnabled = !!s.trigger.enabled;
      triggerSource = s.trigger.source || 'A';
      triggerThreshold = s.trigger.threshold ?? 0;
      triggerDirection = s.trigger.direction || 'rising';
      triggerAutoMs = s.trigger.autoMs ?? 2000;
    }
    if (s.siggen) {
      siggenWave = s.siggen.wave || 'sine';
      siggenFreq = s.siggen.freq ?? 1000;
      siggenAmpMv = s.siggen.amp ?? 1000;
    }
    if (s.display) {
      timePerDivNs = s.display.timePerDivNs ?? 0;
      mathOp = s.display.mathOp || 'none';
      xyMode = !!s.display.xyMode;
      persistenceOn = !!s.display.persistenceOn;
      avgN = s.display.avgN || 1;
      fftOn = !!s.display.fftOn;
      cursorsOn = !!s.display.cursorsOn;
      yCursorsOn = !!s.display.yCursorsOn;
      if (Array.isArray(s.display.measKeys)) {
        measKeys = new Set(s.display.measKeys.filter(k =>
          MEAS_CATALOG.some(m => m.key === k)));
      }
    }
    if (s.streamMode) streamMode = s.streamMode;
    // Apply hardware-relevant settings if connected
    if (connected) {
      updateChannelA(); updateChannelB(); updateTimebase();
      if (triggerEnabled) updateTrigger();
      else DisableTrigger().catch(() => {});
    }
  }
  function handlePresetSave() {
    const name = (presetName || '').trim();
    if (!name) { showError('Preset name required'); return; }
    presets = { ...presets, [name]: snapshotSettings() };
    savePresetsToStorage();
    selectedPreset = name;
    showError('Preset "' + name + '" saved');
  }
  function handlePresetLoad() {
    if (!selectedPreset || !presets[selectedPreset]) return;
    applySettings(presets[selectedPreset]);
    showError('Preset "' + selectedPreset + '" loaded');
  }
  function handlePresetDelete() {
    if (!selectedPreset) return;
    const { [selectedPreset]: _, ...rest } = presets;
    presets = rest;
    savePresetsToStorage();
    selectedPreset = '';
  }

  // Auto-setup: sweep range from widest to narrowest, pick smallest range
  // that doesn't clip; estimate signal frequency via zero-crossings; pick
  // the timebase so one block covers ~3 periods; set trigger at 50 % pp.
  async function handleAutoSetup() {
    if (!connected || isStreaming) return;
    try {
      // 1. Enable CH A only, DC coupling, widest range
      chAEnabled = true; chACoupling = 'DC'; chARange = '20V';
      chBEnabled = false;
      await SetChannelA(true, 'DC', '20V');
      await SetChannelB(false, 'DC', chBRange);
      // Start with a moderate timebase (tb=5 = 320 ns/sample)
      timebase = 5;
      await SetTimebase(5);
      await DisableTrigger();

      const RANGES_ORDER = ['20V','10V','5V','2V','1V','500mV','200mV','100mV','50mV'];
      const RANGE_TO_MV = RANGE_MV; // existing map

      let probe = await CaptureBlock();
      if (!probe || !probe.channelA || probe.channelA.length === 0) {
        showError('Auto-setup: no signal captured');
        return;
      }
      // 2. Find smallest range without clipping using the first probe's pp
      let pp = 0, mn = probe.channelA[0], mx = mn;
      for (const v of probe.channelA) { if (v < mn) mn = v; if (v > mx) mx = v; }
      pp = mx - mn;
      const mean = (mn + mx) / 2;
      const halfAmp = (mx - mn) / 2;
      // Pick the smallest range where full-scale ≥ 1.25 × peak of signal
      let bestRange = '20V';
      for (const r of RANGES_ORDER.slice().reverse()) {
        if (RANGE_TO_MV[r] >= 1.25 * Math.max(Math.abs(mn), Math.abs(mx))) {
          bestRange = r;
          break;
        }
      }
      chARange = bestRange;
      await SetChannelA(true, 'DC', bestRange);
      // 3. Re-capture and estimate frequency
      probe = await CaptureBlock();
      let estHz = 0;
      if (probe && probe.channelA && probe.channelA.length > 16 && probe.timebaseNs > 0) {
        const m = computeMeas(probe.channelA, probe.timebaseNs);
        if (m && m.freqHz > 0) estHz = m.freqHz;
      }
      // 4. Choose timebase so window covers ~3 periods
      if (estHz > 0) {
        const blockNs = 3 * 1e9 / estHz;
        const dtTarget = blockNs / 8064;
        let tb = Math.round(Math.log2(Math.max(1, dtTarget / 10)));
        if (tb < 0) tb = 0;
        if (tb > 15) tb = 15;
        timebase = tb;
        await SetTimebase(tb);
      }
      // 5. Trigger at mean (50 % pp)
      triggerEnabled = true; triggerSource = 'A'; triggerDirection = 'rising';
      triggerThreshold = Math.round(mean); triggerAutoMs = 1000;
      await SetTrigger('A', triggerThreshold, 'rising', 0, triggerAutoMs);
      // 6. Final capture to refresh display
      const finalCap = await CaptureBlock();
      if (finalCap) waveformData = finalCap;
      showError('Auto-setup: range=' + bestRange + ', tb=' + timebase +
                (estHz > 0 ? ', freq≈' + fmtHz(estHz) : ', freq?'));
    } catch (e) {
      showError('Auto-setup failed: ' + e);
    }
  }

  async function handleExportCSV() {
    const sA = visible.samplesA ? Array.from(visible.samplesA) : [];
    const sB = visible.samplesB ? Array.from(visible.samplesB) : [];
    if (sA.length === 0) return;
    try {
      const path = await ExportCSV(sA, sB, viewDtNs || 0);
      if (path) showError('Saved ' + path);
    } catch (e) { showError('Export failed: ' + e); }
  }

  let waveformCanvasRef;
  async function handleExportPNG() {
    if (!waveformCanvasRef || !waveformCanvasRef.snapshotDataURL) return;
    const url = waveformCanvasRef.snapshotDataURL();
    if (!url) return;
    try {
      const path = await ExportPNG(url);
      if (path) showError('Saved ' + path);
    } catch (e) { showError('PNG export failed: ' + e); }
  }

  function mathOpLabel(op) {
    switch (op) {
      case 'add': return 'A + B';
      case 'sub': return 'A − B';
      case 'mul': return 'A × B';
      case 'inva': return '−A';
      case 'invb': return '−B';
      default: return '';
    }
  }

  // Radix-2 Cooley-Tukey FFT on real input. Input is padded/truncated to
  // the nearest power of 2, windowed with Hann, and returned as magnitude
  // in dBFS-normalized-to-1.0. Output length is N/2 (positive frequencies).
  $: fftResult = (fftOn && visible && visible.samplesA && visible.samplesA.length >= 16 && viewDtNs > 0)
    ? fftMag(visible.samplesA, 1e9 / viewDtNs)
    : null;

  let fftCanvas;
  function drawFft() {
    if (!fftCanvas || !fftResult) return;
    const dpr = window.devicePixelRatio || 1;
    const rect = fftCanvas.parentElement.getBoundingClientRect();
    const W = rect.width, H = rect.height;
    fftCanvas.width = W * dpr; fftCanvas.height = H * dpr;
    fftCanvas.style.width = W + 'px'; fftCanvas.style.height = H + 'px';
    const c = fftCanvas.getContext('2d');
    c.scale(dpr, dpr);
    // Background + grid
    c.fillStyle = '#0a0e17'; c.fillRect(0, 0, W, H);
    const ml = 60, mt = 8, mr = 10, mb = 28;
    const gw = W - ml - mr, gh = H - mt - mb;
    c.strokeStyle = '#1e2d3d'; c.lineWidth = 0.5;
    for (let i = 0; i <= 10; i++) {
      const x = ml + gw * i / 10;
      c.beginPath(); c.moveTo(x, mt); c.lineTo(x, mt + gh); c.stroke();
    }
    for (let i = 0; i <= 6; i++) {
      const y = mt + gh * i / 6;
      c.beginPath(); c.moveTo(ml, y); c.lineTo(ml + gw, y); c.stroke();
    }
    c.strokeStyle = '#2a3a4a'; c.lineWidth = 1; c.strokeRect(ml, mt, gw, gh);

    const { freq, mag, fs } = fftResult;
    const fMax = fs / 2;
    const dbMax = 0, dbMin = -100; // display range
    // Labels
    c.fillStyle = '#8899aa'; c.font = '10px monospace';
    c.textAlign = 'right'; c.textBaseline = 'middle';
    for (let i = 0; i <= 6; i++) {
      const db = dbMax - (dbMax - dbMin) * i / 6;
      c.fillText(db.toFixed(0) + ' dB', ml - 5, mt + gh * i / 6);
    }
    c.textAlign = 'center'; c.textBaseline = 'top';
    for (let i = 0; i <= 10; i++) {
      const f = fMax * i / 10;
      let lbl = f >= 1e6 ? (f / 1e6).toFixed(1) + ' MHz'
              : f >= 1e3 ? (f / 1e3).toFixed(1) + ' kHz'
              : f.toFixed(0) + ' Hz';
      c.fillText(lbl, ml + gw * i / 10, mt + gh + 4);
    }
    // Spectrum line
    c.strokeStyle = '#66ddff'; c.lineWidth = 1.2; c.beginPath();
    for (let i = 1; i < freq.length; i++) {
      const x = ml + (freq[i] / fMax) * gw;
      const y = mt + gh * (1 - (mag[i] - dbMin) / (dbMax - dbMin));
      if (i === 1) c.moveTo(x, y); else c.lineTo(x, y);
    }
    c.stroke();
    // Find dominant peak and annotate
    let peakIdx = 1, peakMag = mag[1];
    for (let i = 2; i < mag.length; i++) if (mag[i] > peakMag) { peakMag = mag[i]; peakIdx = i; }
    const pf = freq[peakIdx];
    c.fillStyle = '#ff6ba8';
    c.textAlign = 'left'; c.textBaseline = 'top';
    c.font = 'bold 11px monospace';
    const pfStr = pf >= 1e3 ? (pf / 1e3).toFixed(2) + ' kHz' : pf.toFixed(1) + ' Hz';
    c.fillText('Peak: ' + pfStr + '  (' + peakMag.toFixed(1) + ' dB)', ml + 5, mt + 5);
  }

  $: if (fftOn && fftResult) drawFft();

  /* Ring buffer ops are pure functions in lib/dsp/ring.js; App.svelte owns
   * the (buf, head, len) cursors and wraps push/clear for reactivity. */
  function pushA(arr) {
    if (!arr || !arr.length) return;
    if (!streamRingA) streamRingA = createRing();
    const r = pushRing(streamRingA, streamRingHeadA, streamRingALen, arr);
    streamRingHeadA = r.head; streamRingALen = r.len;
  }
  function pushB(arr) {
    if (!arr || !arr.length) return;
    if (!streamRingB) streamRingB = createRing();
    const r = pushRing(streamRingB, streamRingHeadB, streamRingBLen, arr);
    streamRingHeadB = r.head; streamRingBLen = r.len;
  }
  function clearRings() {
    streamRingHeadA = 0; streamRingHeadB = 0;
    streamRingALen  = 0; streamRingBLen  = 0;
  }

  /** Materialise the *entire* retained buffer for the decoder's "Re-analyse"
   *  button. While streaming, this is the rolling ring (up to RING_CAP); in
   *  single-shot it's the current waveformData. Returns plain Arrays so the
   *  Wails bridge serialises cleanly. */
  function getFullSamplesForDecode() {
    if (isStreaming) {
      const a = materializeRing(streamRingA, streamRingHeadA,
                                streamRingALen, 0, streamRingALen);
      const b = materializeRing(streamRingB, streamRingHeadB,
                                streamRingBLen, 0, streamRingBLen);
      return {
        samplesA: a ? Array.from(a) : [],
        samplesB: b ? Array.from(b) : [],
        dtNs:     viewDtNs,
        rangeMvA: lastStreamRangeMvA,
        rangeMvB: lastStreamRangeMvB,
      };
    }
    if (!waveformData) return null;
    return {
      samplesA: waveformData.channelA || [],
      samplesB: waveformData.channelB || [],
      dtNs:     waveformData.timebaseNs,
      rangeMvA: waveformData.rangeMvA || currentRangeMvA,
      rangeMvB: waveformData.rangeMvB || currentRangeMvB,
    };
  }

  // Cycle timePerDivNs up (in) or down (out) in the preset list.
  // fracX (0..1) = pointer X under the canvas; when zooming in we pin that
  // cursor position by adjusting displayOffsetPct so the point under the
  // cursor stays put.
  function cycleTimeDiv(direction, fracX = 0.5) {
    const presets = TIME_DIV_PRESETS;
    // Find current index (0 = auto handled separately).
    let idx = presets.findIndex(p => p.ns === timePerDivNs);
    if (idx < 0) idx = 0;
    // direction < 0 => zoom in (smaller time/div), > 0 => zoom out.
    let nextIdx;
    if (direction < 0) {
      // If auto, drop to a reasonable starting div based on current buffer span.
      if (idx === 0) {
        const spanNs = currentBufferSpanNs();
        if (spanNs > 0) {
          // Pick next smaller preset such that preset*10 < spanNs.
          nextIdx = 1;
          for (let i = presets.length - 1; i >= 1; i--) {
            if (presets[i].ns * 10 < spanNs) { nextIdx = i; break; }
          }
        } else {
          nextIdx = 1;
        }
      } else {
        nextIdx = Math.max(1, idx - 1);
      }
    } else {
      if (idx === 0) return; // already auto
      nextIdx = idx + 1;
      if (nextIdx >= presets.length) {
        // fall back to auto at max zoom-out
        nextIdx = 0;
      }
    }
    if (nextIdx === idx) return;

    // Zoom-to-cursor: keep time under cursor constant by adjusting offset.
    // Only meaningful when not in auto and not in live-roll streaming.
    const inRoll = isStreaming && !streamPaused;
    if (!inRoll && idx > 0 && nextIdx > 0) {
      const dt = effectiveSampleDtNs();
      const total = totalSampleCount();
      if (dt > 0 && total > 0) {
        const oldSpanN = Math.max(1, Math.round(presets[idx].ns * 10 / dt));
        const newSpanN = Math.max(1, Math.round(presets[nextIdx].ns * 10 / dt));
        const oldStart = Math.round(((total - oldSpanN) * displayOffsetPct) / 100);
        const cursorSample = oldStart + Math.round(oldSpanN * fracX);
        let newStart = cursorSample - Math.round(newSpanN * fracX);
        const maxStart = Math.max(0, total - newSpanN);
        if (newStart < 0) newStart = 0;
        if (newStart > maxStart) newStart = maxStart;
        displayOffsetPct = maxStart > 0 ? (newStart / maxStart) * 100 : 0;
      }
    }
    timePerDivNs = presets[nextIdx].ns;
  }

  function effectiveSampleDtNs() {
    if (isStreaming) {
      return effectiveDtNs > 0 ? effectiveDtNs : lastStreamTimebaseNs;
    }
    return waveformData ? waveformData.timebaseNs : 0;
  }

  function totalSampleCount() {
    if (isStreaming) {
      return Math.max(streamRingALen, streamRingBLen);
    }
    return waveformData ? (waveformData.channelA?.length || waveformData.channelB?.length || 0) : 0;
  }

  function currentBufferSpanNs() {
    const dt = effectiveSampleDtNs();
    const n = totalSampleCount();
    return dt > 0 && n > 0 ? dt * n : 0;
  }

  // Event listeners
  let unsubWaveform, unsubStats, unsubStopped;

  onMount(() => {
    // Surface any uncaught JS error as a toast + console log. Without this,
    // WebKitGTK silently kills the renderer on some errors so we never find
    // out what went wrong.
    window.addEventListener('error', (ev) => {
      console.error('[window error]', ev.error || ev.message);
      showError('JS error: ' + (ev.error?.message || ev.message || 'unknown'));
    });
    window.addEventListener('unhandledrejection', (ev) => {
      console.error('[unhandled promise]', ev.reason);
      showError('Async error: ' + (ev.reason?.message || String(ev.reason)));
    });

    unsubWaveform = EventsOn('waveform', (data) => {
      if (!data) return;
      if (isStreaming) {
        // Remember per-event metadata so the slice + canvas can scale/label.
        if (data.timebaseNs) lastStreamTimebaseNs = data.timebaseNs;
        if (data.rangeMvA)   lastStreamRangeMvA = data.rangeMvA;
        if (data.rangeMvB)   lastStreamRangeMvB = data.rangeMvB;
        if (!streamPaused) {
          pushA(data.channelA);
          pushB(data.channelB);
        }
        // Keep waveformData around so single-shot fallbacks still work.
        waveformData = data;
      } else {
        waveformData = data;
      }
    });
    unsubStats = EventsOn('streamStats', (stats) => {
      streamStats = stats;
      // Wall-clock per-sample spacing from measured throughput — compensates
      // for inter-block gaps that make timebaseNs*N overestimate the time span.
      if (stats && stats.samplesPerSec > 0) {
        effectiveDtNs = 1e9 / stats.samplesPerSec;
      }
    });
    // Backend emits this when the C streaming thread dies on its own
    // (fatal USB error, allocation failure, etc.). Flip the UI out of
    // streaming mode so the user can act on the error.
    unsubStopped = EventsOn('streamStopped', (info) => {
      isStreaming = false;
      streamPaused = false;
      const reason = (info && info.reason) ? info.reason : 'unknown';
      showError('Streaming stopped: ' + reason);
    });

    GetAllTimebases().then(tb => { timebases = tb; }).catch(() => {});
    loadPresetsFromStorage();

    // First-run check: if the firmware isn't on disk, pop the setup dialog
    // before the user tries to connect. They can close it and come back
    // later via the Firmware button.
    FirmwareStatus().then(s => {
      fwInstalled = !!(s && s.installed);
      if (!fwInstalled) fwModalOpen = true;
    }).catch(() => {});
  });

  onDestroy(() => {
    if (unsubWaveform) unsubWaveform();
    if (unsubStats) unsubStats();
    if (unsubStopped) unsubStopped();
  });

  $: currentRangeMvA = RANGE_MV[chARange] || 5000;
  $: currentRangeMvB = RANGE_MV[chBRange] || 5000;
  $: tbLabel = timebases[timebase] ? timebases[timebase].label : '';

  // Choose the visible per-sample spacing (wall-clock while streaming).
  // Per-sample time spacing: ALWAYS the timebase's nominal dt (10 * 2^TB ns).
  // Scope convention — cursors and frequency measurements need to reflect
  // signal time, not wall-clock acquisition rate (which includes block dead
  // time in fast-streaming). `effectiveDtNs` is kept only for the rate
  // display in the status bar.
  $: viewDtNs = isStreaming
       ? (lastStreamTimebaseNs > 0 ? lastStreamTimebaseNs : effectiveDtNs)
       : (waveformData ? waveformData.timebaseNs : 0);

  // Slice the correct chunk of data for the canvas.
  // - streaming + unpaused: last N samples (roll mode)
  // - otherwise: N samples starting at displayOffsetPct * (total-N)
  // - timePerDivNs == 0 (auto): full buffer, no slicing
  $: visible = (() => {
    // Pick source arrays.
    let srcA, srcB, rangeA, rangeB;
    if (isStreaming) {
      srcA = streamRingA;
      srcB = streamRingB;
      rangeA = lastStreamRangeMvA;
      rangeB = lastStreamRangeMvB;
    } else if (waveformData) {
      srcA = waveformData.channelA || [];
      srcB = waveformData.channelB || [];
      rangeA = waveformData.rangeMvA || currentRangeMvA;
      rangeB = waveformData.rangeMvB || currentRangeMvB;
    } else {
      return { samplesA: null, samplesB: null, startTimeNs: 0, spanNs: 0,
               rangeMvA: currentRangeMvA, rangeMvB: currentRangeMvB };
    }
    const nA = isStreaming ? streamRingALen : (srcA ? srcA.length : 0);
    const nB = isStreaming ? streamRingBLen : (srcB ? srcB.length : 0);
    const total = Math.max(nA, nB);
    if (total === 0) {
      return { samplesA: null, samplesB: null, startTimeNs: 0, spanNs: 0,
               rangeMvA: rangeA, rangeMvB: rangeB };
    }
    const dt = viewDtNs > 0 ? viewDtNs : 0;

    /** Unified slicer that handles three cases:
     *   - streaming: ring is circular → materialize a fresh Float32Array
     *   - Float32Array (waveformData path): subarray (zero-copy)
     *   - plain Array (waveformData.channelA from Wails): shallow slice */
    const sliceOf = (which, arr, len, start, end) => {
      if (!arr || len <= 0) return null;
      const s = Math.max(0, Math.min(start, len));
      const e = Math.max(s, Math.min(end, len));
      if (e === s) return null;
      if (isStreaming) {
        const head = which === 'A' ? streamRingHeadA : streamRingHeadB;
        return materializeRing(arr, head, len, s, e);
      }
      if (typeof arr.subarray === 'function') return arr.subarray(s, e);
      return arr.slice(s, e);
    };

    // Auto: show the whole buffer.
    if (timePerDivNs === 0) {
      return {
        samplesA: sliceOf('A', srcA, nA, 0, nA),
        samplesB: sliceOf('B', srcB, nB, 0, nB),
        startTimeNs: 0,
        spanNs: dt * total,
        rangeMvA: rangeA,
        rangeMvB: rangeB,
      };
    }

    // Fixed time/div: compute N samples for 10 divisions.
    const spanNs = timePerDivNs * 10;
    let windowN = dt > 0 ? Math.max(1, Math.round(spanNs / dt)) : total;
    if (windowN > total) windowN = total;

    // Start index selection.
    let start;
    if (isStreaming && !streamPaused) {
      // Roll mode: always trailing edge.
      start = total - windowN;
    } else {
      const maxStart = Math.max(0, total - windowN);
      start = Math.round((displayOffsetPct / 100) * maxStart);
      if (start < 0) start = 0;
      if (start > maxStart) start = maxStart;
    }
    const end = start + windowN;

    const sliceA = sliceOf('A', srcA, nA, start, end);
    const sliceB = sliceOf('B', srcB, nB, start, end);

    return {
      samplesA: sliceA && sliceA.length ? sliceA : null,
      samplesB: sliceB && sliceB.length ? sliceB : null,
      startTimeNs: dt * start,
      spanNs: dt * windowN,
      rangeMvA: rangeA,
      rangeMvB: rangeB,
    };
  })();

  // Full scope-style measurements: min, max, mean, pp, RMS, frequency,
  // period, duty cycle — estimated over the visible slice with simple
  // zero-crossing detection (with hysteresis to resist DAC noise).
  $: mathTrace = computeMath(mathOp, visible.samplesA, visible.samplesB);

  $: {
    const v = visible;
    const dt = viewDtNs;
    measA = (v && v.samplesA && chAEnabled) ? computeMeas(v.samplesA, dt) : null;
    measB = (v && v.samplesB && chBEnabled) ? computeMeas(v.samplesB, dt) : null;
    measM = (mathTrace) ? computeMeas(mathTrace, dt) : null;
  }

  // ---- Rolling statistics (min/max/avg of each measurement over last N frames).
  let statsEnabled = false;
  let statsA = makeEmptyStats();
  let statsB = makeEmptyStats();
  $: if (statsEnabled) { pushStat(statsA, measA); pushStat(statsB, measB); }
  $: statsADisplay = statsEnabled ? aggregateStatsBag(statsA) : null;
  $: statsBDisplay = statsEnabled ? aggregateStatsBag(statsB) : null;
  function resetStats() { statsA = makeEmptyStats(); statsB = makeEmptyStats(); }

  // --- Controls wiring ---
  function onCanvasZoom(ev) {
    const { deltaY, fracX } = ev.detail;
    cycleTimeDiv(deltaY < 0 ? -1 : 1, fracX);
  }
  function onCanvasPan(ev) {
    if (isStreaming && !streamPaused) return; // roll mode ignores pan
    const { dxFrac } = ev.detail;
    // Pan 1 full view = 100% of offset range. Drag right => show earlier data.
    const pct = displayOffsetPct - dxFrac * 100;
    displayOffsetPct = Math.max(0, Math.min(100, pct));
  }
  // Pause streaming + freeze trailing edge *before* the box-zoom drag starts.
  // Why: in roll mode the ring keeps advancing under the cursor, so the data
  // visible at pointerdown is not the data still on screen at pointerup, and
  // the zoom lands on the wrong region.
  function onCanvasZoomBegin() {
    if (isStreaming && !streamPaused) {
      streamPaused = true;
      displayOffsetPct = 100;
    }
  }
  // Box-zoom from canvas: reframe so the dragged region fills the view.
  // Uses free-form Time/div (not snapped to preset) so the selection keeps
  // its exact width and start — snapping would shift the framing by up to
  // 2.5× and was causing the "signal disappears to the right" bug.
  //
  // CRITICAL: use `viewDtNs` (same dt the `visible` slice was computed with),
  // NOT `effectiveSampleDtNs()`. During Fast streaming those two diverge
  // (wall-clock vs nominal timebase), which made `newStartNs / dt` land on
  // the wrong sample index and pushed the zoom region off to the right.
  function onCanvasZoomTo(ev) {
    const { startFrac, endFrac } = ev.detail;
    if (!(endFrac > startFrac)) return;
    const curStartNs = visible.startTimeNs || 0;
    const curSpanNs  = visible.spanNs || 0;
    if (curSpanNs <= 0) return;
    const newSpanNs = curSpanNs * (endFrac - startFrac);
    const newStartNs = curStartNs + curSpanNs * startFrac;
    timePerDivNs = newSpanNs / 10;
    const dt = viewDtNs;
    const total = totalSampleCount();
    if (dt <= 0 || total <= 0) return;
    const windowN  = Math.max(1, Math.round(timePerDivNs * 10 / dt));
    const maxStart = Math.max(0, total - windowN);
    const targetSampleStart = Math.round(newStartNs / dt);
    const clamped = Math.max(0, Math.min(maxStart, targetSampleStart));
    displayOffsetPct = maxStart > 0 ? (clamped / maxStart) * 100 : 0;
  }
  function shiftDiv(n) {
    // One division = 10% of view; move by one preset division.
    if (timePerDivNs === 0) return;
    const dt = effectiveSampleDtNs();
    const total = totalSampleCount();
    if (dt <= 0 || total <= 0) return;
    const windowN = Math.max(1, Math.round(timePerDivNs * 10 / dt));
    const maxStart = Math.max(1, total - windowN);
    const divSamples = Math.max(1, Math.round(windowN / 10));
    const curStart = Math.round((displayOffsetPct / 100) * maxStart);
    const newStart = Math.max(0, Math.min(maxStart, curStart + n * divSamples));
    displayOffsetPct = (newStart / maxStart) * 100;
  }
  function fitView() {
    timePerDivNs = 0;
    displayOffsetPct = isStreaming ? 100 : 0;
  }
  function togglePause() {
    streamPaused = !streamPaused;
    if (streamPaused) {
      // Freeze cursor at trailing edge so slider starts at the right.
      displayOffsetPct = 100;
    }
  }
  // Hide offset slider in live roll mode (streaming + not paused).
  $: offsetActive = !(isStreaming && !streamPaused) && timePerDivNs > 0;

  // Dual-range slider: two handles that describe the visible window as
  // percentages of the full buffer. Derived reactively from the existing
  // (timePerDivNs, displayOffsetPct) state; writes back through setWindowPct.
  $: dualRangeActive = totalSampleCount() > 0 && viewDtNs > 0
                       && (timePerDivNs > 0 || streamPaused || !isStreaming);
  $: windowStartPct = (() => {
    const total = totalSampleCount();
    const dt = viewDtNs;
    if (total <= 0 || dt <= 0) return 0;
    if (timePerDivNs <= 0) return 0;
    const windowN = Math.max(1, Math.round(timePerDivNs * 10 / dt));
    const maxStart = Math.max(0, total - windowN);
    const startSample = (displayOffsetPct / 100) * maxStart;
    return Math.max(0, Math.min(100, (startSample / total) * 100));
  })();
  $: windowEndPct = (() => {
    const total = totalSampleCount();
    const dt = viewDtNs;
    if (total <= 0 || dt <= 0) return 100;
    if (timePerDivNs <= 0) return 100;
    const windowN = Math.max(1, Math.round(timePerDivNs * 10 / dt));
    const maxStart = Math.max(0, total - windowN);
    const startSample = (displayOffsetPct / 100) * maxStart;
    return Math.max(0, Math.min(100, ((startSample + windowN) / total) * 100));
  })();
  // Set the visible window from a (startPct, endPct) pair. Auto-pauses
  // streaming on change since a narrowed window is meaningless in roll mode.
  // Uses `viewDtNs` (same dt the visible slice uses) — not effectiveSampleDtNs,
  // which tracks wall-clock rate and would land the window on wrong samples.
  function setWindowPct(startPct, endPct) {
    const MIN_WIDTH_PCT = 0.5; // don't let the window collapse to zero
    let s = Math.max(0, Math.min(100 - MIN_WIDTH_PCT, startPct));
    let e = Math.max(s + MIN_WIDTH_PCT, Math.min(100, endPct));
    if (isStreaming && !streamPaused) streamPaused = true;
    const dt = viewDtNs;
    const total = totalSampleCount();
    if (dt <= 0 || total <= 0) return;
    const totalNs = total * dt;
    const newSpanNs = ((e - s) / 100) * totalNs;
    timePerDivNs = newSpanNs / 10;
    const windowN = Math.max(1, Math.round(timePerDivNs * 10 / dt));
    const maxStart = Math.max(0, total - windowN);
    const targetStartSample = (s / 100) * total;
    const clamped = Math.max(0, Math.min(maxStart, targetStartSample));
    displayOffsetPct = maxStart > 0 ? (clamped / maxStart) * 100 : 0;
  }
  function onRangeMinInput(ev) {
    const v = +ev.target.value;
    setWindowPct(Math.min(v, windowEndPct - 0.5), windowEndPct);
  }
  function onRangeMaxInput(ev) {
    const v = +ev.target.value;
    setWindowPct(windowStartPct, Math.max(v, windowStartPct + 0.5));
  }

  // Custom Time/div dropdown (native <select> has an auto-select-on-release
  // bug in WebKit2GTK when the menu opens directly under the cursor).
  let tdOpen = false;
  let tdButton;
  $: tdLabel = (() => {
    if (timePerDivNs === 0) return 'Auto';
    const preset = TIME_DIV_PRESETS.find(p => p.ns === timePerDivNs);
    if (preset) return preset.label;
    return fmtTime(timePerDivNs);
  })();
  function tdToggle(ev) {
    ev.stopPropagation();
    tdOpen = !tdOpen;
  }
  function tdPick(ns, ev) {
    ev.stopPropagation();
    timePerDivNs = ns;
    tdOpen = false;
  }
  function tdDocClick(ev) {
    if (!tdOpen) return;
    if (tdButton && !tdButton.contains(ev.target)) tdOpen = false;
  }
</script>

<svelte:window on:click={tdDocClick} />

<div class="app-layout">
  <!-- Toolbar -->
  <div class="toolbar">
    <span class="title">PicoScope 2204A</span>
    <div class="separator"></div>

    <button class="btn" class:btn-primary={!connected && fwInstalled}
            class:btn-danger={connected}
            on:click={handleConnect} disabled={connecting || !fwInstalled}
            title={fwInstalled ? '' : 'Firmware not installed — click the Firmware button first'}>
      {#if connecting}Connecting...{:else if connected}Disconnect{:else}Connect{/if}
    </button>

    <button class="btn" class:btn-primary={!fwInstalled}
            on:click={() => fwModalOpen = true}
            title="Extract/check the scope firmware">
      {fwInstalled ? 'Firmware' : 'Firmware ✗'}
    </button>

    <div class="separator"></div>

    <button class="btn" on:click={handleCapture}
            disabled={!connected || isStreaming || capturing}>
      {#if capturing}Capturing...{:else}Single Shot{/if}
    </button>

    <!-- Streaming mode selector (disabled while streaming) -->
    <select class="select-inline" bind:value={streamMode}
            disabled={isStreaming} title="Streaming mode">
      <option value="fast">Fast block (330 kS/s)</option>
      <option value="sdk">SDK continuous (1 MS/s, gap-free)</option>
      <option value="native">Native FPGA (~100 S/s)</option>
    </select>

    {#if isStreaming}
      <button class="btn btn-danger" on:click={handleStopStreaming}>
        Stop
      </button>
    {:else}
      <button class="btn btn-streaming" on:click={handleStartStreaming}
              disabled={!connected}>
        Run
      </button>
    {/if}
  </div>

  <!-- Main area -->
  <div class="main-area">
    <!-- Sidebar -->
    <div class="sidebar">
      <ChannelPanel label="Channel A" dotColor="#00ff88" ranges={RANGES} panelOpen
                    bind:enabled={chAEnabled} bind:coupling={chACoupling}
                    bind:range={chARange} bind:offsetMv={offsetMvA} bind:vdivMv={vdivMvA}
                    onChange={updateChannelA} />

      <ChannelPanel label="Channel B" dotColor="#ffd700" ranges={RANGES}
                    bind:enabled={chBEnabled} bind:coupling={chBCoupling}
                    bind:range={chBRange} bind:offsetMv={offsetMvB} bind:vdivMv={vdivMvB}
                    onChange={updateChannelB} />

      <!-- Timebase -->
      <TimebaseControls
        bind:timebase bind:samples
        {timebases} {maxSamples} {tbLabel} {connected}
        dual={chAEnabled && chBEnabled}
        onTimebaseChange={updateTimebase}
        onSamplesChange={updateSamples} />

      <!-- Trigger -->
      <TriggerControls
        bind:enabled={triggerEnabled}
        bind:source={triggerSource}
        bind:threshold={triggerThreshold}
        bind:direction={triggerDirection}
        bind:autoMs={triggerAutoMs}
        onChange={updateTrigger} />

      <!-- Signal Generator -->
      <SiggenControls
        waveTypes={WAVE_TYPES}
        bind:wave={siggenWave}
        bind:freq={siggenFreq}
        bind:ampMv={siggenAmpMv}
        bind:offsetMv={siggenOffsetMv}
        bind:duty={siggenDuty}
        bind:sweep={siggenSweep}
        bind:stopFreq={siggenStopFreq}
        bind:sweepInc={siggenSweepInc}
        bind:sweepDwell={siggenSweepDwell}
        {connected}
        enabled={siggenEnabled}
        onEnable={handleSiggenOn}
        onDisable={handleSiggenOff} />

      <!-- Protocol Decoder -->
      <details class="panel">
        <summary>Protocol Decoder</summary>
        <div class="panel-content">
          <DecoderPanel
            samplesA={visible.samplesA}
            samplesB={visible.samplesB}
            rangeMvA={visible.rangeMvA}
            rangeMvB={visible.rangeMvB}
            dtNs={viewDtNs}
            isStreaming={isStreaming}
            paused={streamPaused}
            sliceStartNs={visible.startTimeNs || 0}
            getFullSamples={getFullSamplesForDecode}
            on:decode={(ev) => {
              decoderState = ev.detail;
              decoderAnnotations = ev.detail.events || [];
            }}
          />
        </div>
      </details>

      <!-- Calibration -->
      <CalibrationPanel
        ranges={RANGES}
        bind:range={calRange}
        bind:offsetMv={calOffsetMv}
        bind:gain={calGain}
        {connected} {isStreaming}
        onOpenEditor={() => calModalOpen = true}
        onAutoCalibrate={handleCalibrateDCOffset}
        onApply={handleSetRangeCal} />

      <!-- Presets -->
      <PresetsPanel
        {presets}
        bind:presetName
        bind:selected={selectedPreset}
        onSave={handlePresetSave}
        onLoad={handlePresetLoad}
        onDelete={handlePresetDelete} />

      <!-- Diagnostics -->
      <DiagnosticsPanel {connected} {isStreaming} {rawPreview}
                        onCaptureRaw={handleCaptureRaw} />
    </div>

    <!-- Waveform display -->
    <div class="waveform-area">
      <div class="waveform-display" class:with-fft={fftOn}>
        <WaveformCanvas
          bind:this={waveformCanvasRef}
          samplesA={visible.samplesA}
          samplesB={visible.samplesB}
          samplesM={mathTrace}
          mathLabel={mathOpLabel(mathOp)}
          channelAEnabled={chAEnabled}
          channelBEnabled={chBEnabled}
          rangeMvA={visible.rangeMvA}
          rangeMvB={visible.rangeMvB}
          startTimeNs={visible.startTimeNs}
          spanNs={visible.spanNs}
          xyMode={xyMode}
          cursorsOn={cursorsOn}
          cursor1Pct={cursor1Pct}
          cursor2Pct={cursor2Pct}
          yCursorsOn={yCursorsOn}
          yCursor1Mv={yCursor1Mv}
          yCursor2Mv={yCursor2Mv}
          offsetMvA={offsetMvA}
          offsetMvB={offsetMvB}
          vdivMvA={vdivMvA}
          vdivMvB={vdivMvB}
          persistenceOn={persistenceOn}
          annotations={decoderAnnotations}
          on:zoom={onCanvasZoom}
          on:pan={onCanvasPan}
          on:zoomBegin={onCanvasZoomBegin}
          on:zoomTo={onCanvasZoomTo}
          on:cursormove={(ev) => {
            if (ev.detail.which === 1) cursor1Pct = ev.detail.pct;
            else if (ev.detail.which === 2) cursor2Pct = ev.detail.pct;
          }}
          on:ycursormove={(ev) => {
            if (ev.detail.which === 1) yCursor1Mv = ev.detail.mv;
            else if (ev.detail.which === 2) yCursor2Mv = ev.detail.mv;
          }}
        />
      </div>

      {#if fftOn}
        <div class="fft-display">
          <canvas bind:this={fftCanvas}></canvas>
        </div>
      {/if}

      <!-- Scope controls strip -->
      <DisplayControls
        presets={TIME_DIV_PRESETS}
        {timePerDivNs} {tdLabel} {tdOpen}
        bind:tdButton
        {offsetActive} {dualRangeActive} {windowStartPct} {windowEndPct}
        spanNs={visible.spanNs}
        {isStreaming} {streamPaused}
        onTdToggle={tdToggle}
        onTdPick={tdPick}
        onShiftDiv={shiftDiv}
        onTogglePause={togglePause}
        onFit={fitView}
        {onRangeMinInput} {onRangeMaxInput} />

      <!-- Analysis controls: math, cursors, XY, persistence, averaging, FFT -->
      <AnalysisControls
        bind:mathOp bind:cursorsOn bind:yCursorsOn bind:xyMode
        bind:persistenceOn bind:avgN bind:fftOn
        bind:measMenuOpen bind:measKeys
        statsEnabled={statsEnabled}
        catalog={MEAS_CATALOG}
        measDefault={MEAS_DEFAULT}
        canExportCsv={!!(visible.samplesA && visible.samplesA.length)}
        {connected} {isStreaming}
        onToggleStats={() => { statsEnabled = !statsEnabled; if (!statsEnabled) resetStats(); }}
        onToggleMeasKey={toggleMeasKey}
        onExportCSV={handleExportCSV}
        onExportPNG={handleExportPNG}
        onAuto={handleAutoSetup} />

      <MeasurementsBar {measA} {measB} {measM} {measKeys}
                       catalog={MEAS_CATALOG}
                       {statsADisplay} {statsBDisplay} />
    </div>
  </div>

  <!-- Decoder log (bottom, full width) -->
  <DecoderLog {decoderState} bind:collapsed={decoderLogCollapsed} />

  <!-- Calibration modal (overlay) -->
  <CalibrationModal bind:open={calModalOpen} on:close={() => calModalOpen = false} />
  <FirmwareSetupModal bind:open={fwModalOpen}
                      on:close={async () => {
                        fwModalOpen = false;
                        try {
                          const s = await FirmwareStatus();
                          fwInstalled = !!(s && s.installed);
                        } catch(e) {}
                      }} />

  <StatusBar {connected} {serial} {calDate} {isStreaming} {streamStats}
             {waveformData} {timebases} />
</div>

<ErrorToast message={errorMsg} />
