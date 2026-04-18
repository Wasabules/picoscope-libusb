<script>
  import { onMount, onDestroy } from 'svelte';
  import WaveformCanvas from './lib/WaveformCanvas.svelte';
  import CalibrationModal from './lib/CalibrationModal.svelte';
  import FirmwareSetupModal from './lib/FirmwareSetupModal.svelte';
  import DecoderPanel from './lib/DecoderPanel.svelte';
  import DecoderLog from './lib/DecoderLog.svelte';
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
  function fmtMeas(fmt, v) {
    if (v == null || !isFinite(v)) return '—';
    if (fmt === 'mv')   return fmtMv(v);
    if (fmt === 'hz')   return fmtHz(v);
    if (fmt === 'time') return fmtTime(v);
    if (fmt === 'pct')  return v.toFixed(1) + '%';
    return String(v);
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
  // the canvas in the previous implementation). 1 M samples ≈ 1.3 s of Fast-
  // streaming history (≈ 780 kS/s) or ~2.9 hours at Native (~100 S/s). Visible
  // slices are materialized on demand below.
  // 4M samples ≈ 5.37 s at fast-streaming dt=1280 ns, enough to cover up to
  // 500 ms/div × 10 divisions. At 2 channels × 4 bytes that's 32 MB — fine.
  const RING_CAP = 4_194_304;
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

  // Signal generator
  async function handleSiggenOn() {
    if (!connected) return;
    try {
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
    } catch (e) { showError(String(e)); }
  }

  async function handleSiggenOff() {
    if (!connected) return;
    try {
      await DisableSiggen();
      siggenEnabled = false;
    } catch (e) { showError(String(e)); }
  }

  function fmtMv(v) {
    if (v === null || v === undefined) return '--';
    if (Math.abs(v) >= 1000) return (v / 1000).toFixed(2) + ' V';
    return v.toFixed(1) + ' mV';
  }
  function fmtHz(v) {
    if (!v || !isFinite(v) || v <= 0) return '--';
    if (v >= 1e6) return (v / 1e6).toFixed(2) + ' MHz';
    if (v >= 1e3) return (v / 1e3).toFixed(2) + ' kHz';
    return v.toFixed(1) + ' Hz';
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
  function fftMag(samples, fs) {
    if (!samples || samples.length < 16) return null;
    // Pick N = largest power of 2 <= samples.length, cap at 8192 (plenty for scope).
    let N = 1;
    while (N * 2 <= samples.length && N * 2 <= 8192) N *= 2;
    if (N < 16) return null;
    // Window (Hann) + copy to complex arrays
    const re = new Float64Array(N);
    const im = new Float64Array(N);
    for (let i = 0; i < N; i++) {
      const w = 0.5 * (1 - Math.cos(2 * Math.PI * i / (N - 1)));
      re[i] = samples[i] * w;
    }
    // Bit-reverse permutation
    for (let i = 1, j = 0; i < N; i++) {
      let bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    // Butterflies
    for (let size = 2; size <= N; size *= 2) {
      const half = size / 2;
      const ang = -2 * Math.PI / size;
      for (let i = 0; i < N; i += size) {
        for (let k = 0; k < half; k++) {
          const c = Math.cos(ang * k), s = Math.sin(ang * k);
          const tr = c * re[i + k + half] - s * im[i + k + half];
          const ti = s * re[i + k + half] + c * im[i + k + half];
          re[i + k + half] = re[i + k] - tr;
          im[i + k + half] = im[i + k] - ti;
          re[i + k] += tr;
          im[i + k] += ti;
        }
      }
    }
    // Magnitude (dB, normalized so pure sinusoid at full-scale ≈ 0 dB)
    const M = N / 2;
    const mag = new Float64Array(M);
    const freq = new Float64Array(M);
    const norm = 2 / N;
    for (let i = 0; i < M; i++) {
      const m = Math.sqrt(re[i] * re[i] + im[i] * im[i]) * norm;
      mag[i] = 20 * Math.log10(Math.max(m, 1e-6));
      freq[i] = i * fs / N;
    }
    return { freq, mag, n: N, fs };
  }

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

  function fmtRate(v) {
    if (!v) return '--';
    if (v >= 1e6) return (v / 1e6).toFixed(1) + ' MS/s';
    if (v >= 1e3) return (v / 1e3).toFixed(0) + ' kS/s';
    return v.toFixed(0) + ' S/s';
  }

  function fmtCount(v) {
    if (!v) return '0';
    if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
    if (v >= 1e3) return (v / 1e3).toFixed(1) + 'k';
    return String(v);
  }

  function fmtTime(ns) {
    const abs = Math.abs(ns);
    if (abs < 1e3)  return ns.toFixed(0) + ' ns';
    if (abs < 1e6)  return (ns / 1e3).toFixed(abs < 1e4 ? 2 : 1) + ' µs';
    if (abs < 1e9)  return (ns / 1e6).toFixed(abs < 1e7 ? 2 : 1) + ' ms';
    return (ns / 1e9).toFixed(abs < 1e10 ? 2 : 1) + ' s';
  }

  /* Circular ring helpers. The backing Float32Array is allocated once and
   * reused; only the (head, len) cursors are updated on each push, so GC
   * stays quiet even at 55+ blocks/s. */
  function ensureRings() {
    if (!streamRingA) streamRingA = new Float32Array(RING_CAP);
    if (!streamRingB) streamRingB = new Float32Array(RING_CAP);
  }

  function pushRing(which, arr) {
    if (!arr || arr.length === 0) return;
    ensureRings();
    const buf = which === 'A' ? streamRingA : streamRingB;
    let head  = which === 'A' ? streamRingHeadA : streamRingHeadB;
    let len   = which === 'A' ? streamRingALen  : streamRingBLen;
    const n = arr.length;
    if (n >= RING_CAP) {
      // Incoming block is larger than the ring — keep only its tail.
      const start = n - RING_CAP;
      for (let i = 0; i < RING_CAP; i++) buf[i] = arr[start + i];
      head = 0; len = RING_CAP;
    } else {
      const firstLen = Math.min(n, RING_CAP - head);
      for (let i = 0; i < firstLen; i++) buf[head + i] = arr[i];
      const rem = n - firstLen;
      for (let i = 0; i < rem; i++) buf[i] = arr[firstLen + i];
      head = (head + n) % RING_CAP;
      len  = Math.min(RING_CAP, len + n);
    }
    if (which === 'A') { streamRingHeadA = head; streamRingALen = len; }
    else               { streamRingHeadB = head; streamRingBLen = len; }
  }

  // Cap materialized slice length. The canvas is ~1200 px wide and draws a
  // min/max envelope per column, so anything above ~4 samples/px wastes GC
  // budget with no visual benefit. 16 k output samples keeps the allocation
  // small (64 kB per channel per frame) while preserving every spike via
  // interleaved max/min pairs. Without this cap, a 4 M-sample slice churns
  // 16 MB per frame per channel and the event loop stalls/crashes.
  const MATERIALIZE_MAX = 16_384;

  /** Copy logical samples [start, end) out of the circular ring into a
   *  fresh Float32Array (chronological: index 0 = oldest in the slice).
   *  When the requested range is much larger than MATERIALIZE_MAX, we return
   *  a min/max-envelope compaction — the canvas's own per-column envelope
   *  logic will still reconstruct the trace identically from it. */
  function materializeRing(buf, head, len, start, end) {
    if (!buf || len <= 0) return null;
    start = Math.max(0, Math.min(start, len));
    end   = Math.max(start, Math.min(end, len));
    const n = end - start;
    if (n === 0) return null;
    const oldest = ((head - len) % RING_CAP + RING_CAP) % RING_CAP;

    if (n <= MATERIALIZE_MAX) {
      const readStart = (oldest + start) % RING_CAP;
      const out = new Float32Array(n);
      const firstLen = Math.min(n, RING_CAP - readStart);
      out.set(buf.subarray(readStart, readStart + firstLen), 0);
      if (firstLen < n) out.set(buf.subarray(0, n - firstLen), firstLen);
      return out;
    }

    // Downsample: MATERIALIZE_MAX/2 buckets, each emitting [max, min] so the
    // canvas envelope preserves the signal extremes.
    const buckets = MATERIALIZE_MAX >> 1;
    const out = new Float32Array(buckets * 2);
    const baseIdx = oldest + start;
    for (let b = 0; b < buckets; b++) {
      const sStart = Math.floor((b * n) / buckets);
      const sEnd   = Math.floor(((b + 1) * n) / buckets);
      let mn = Infinity, mx = -Infinity;
      for (let i = sStart; i < sEnd; i++) {
        const v = buf[(baseIdx + i) % RING_CAP];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
      if (mn === Infinity) { mn = 0; mx = 0; }
      out[2 * b]     = mx;
      out[2 * b + 1] = mn;
    }
    return out;
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
          pushRing('A', data.channelA);
          pushRing('B', data.channelB);
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
  function computeMeas(d, dtNs) {
    if (!d || d.length < 4 || !dtNs || dtNs <= 0) return null;
    let mn = d[0], mx = d[0], sum = 0;
    for (let i = 0; i < d.length; i++) {
      if (d[i] < mn) mn = d[i];
      if (d[i] > mx) mx = d[i];
      sum += d[i];
    }
    const mean = sum / d.length;
    let sqsum = 0;
    for (let i = 0; i < d.length; i++) sqsum += (d[i] - mean) * (d[i] - mean);
    const rms = Math.sqrt(sqsum / d.length);

    // Frequency via hysteresis rising-edge detection. Schmitt trigger: we
    // only register a rising edge when the signal crosses highT AFTER
    // having been below lowT (5 % hysteresis on each side of the mean).
    const hyst = Math.max(1, (mx - mn) * 0.05);
    const highT = mean + hyst;
    const lowT  = mean - hyst;
    const rising = [];
    let state = 0;           // +1 = above highT, -1 = below lowT, 0 = between
    let nHigh = 0, nLow = 0; // for duty cycle
    for (let i = 0; i < d.length; i++) {
      const v = d[i];
      if (v >= highT) {
        if (state === -1) rising.push(i);  // low → high crossing
        state = 1; nHigh++;
      } else if (v <= lowT) {
        state = -1; nLow++;
      }
    }
    let freqHz = 0, periodNs = 0;
    if (rising.length >= 2) {
      const spanSamples = rising[rising.length - 1] - rising[0];
      const periods = rising.length - 1;
      const periodSamp = spanSamples / periods;
      periodNs = periodSamp * dtNs;
      if (periodNs > 0) freqHz = 1e9 / periodNs;
    }
    const totalActive = nHigh + nLow;
    const duty = totalActive > 0 ? 100 * nHigh / totalActive : 0;

    // Rise/fall time: find the first clean edge, measure 10-90% crossings.
    // "Clean" = amplitude already swings at least 20 % of pp between lowT
    // and highT, so noisy DC signals don't register spurious sub-ms times.
    const { rise: riseNs, fall: fallNs } = edgeTimes(d, mn, mx, dtNs);

    return { min: mn, max: mx, mean, vpp: mx - mn, rms, freqHz, periodNs,
             duty, riseNs, fallNs };
  }

  function edgeTimes(d, mn, mx, dtNs) {
    const pp = mx - mn;
    if (pp < 10 || d.length < 4) return { rise: 0, fall: 0 };
    const lvl10 = mn + 0.10 * pp;
    const lvl90 = mn + 0.90 * pp;
    // Fractional linear-interpolation helper between sample i-1 and i.
    const crossAt = (i, lvl) => {
      const a = d[i-1], b = d[i];
      if (b === a) return i;
      return (i - 1) + (lvl - a) / (b - a);
    };
    let rise = 0, fall = 0;
    // --- Rise: find first sample <= lvl10, then next > lvl90 ---
    let found10 = -1;
    for (let i = 1; i < d.length; i++) {
      if (found10 < 0) {
        if (d[i-1] <= lvl10 && d[i] > lvl10) found10 = crossAt(i, lvl10);
      } else {
        if (d[i-1] < lvl90 && d[i] >= lvl90) {
          rise = (crossAt(i, lvl90) - found10) * dtNs;
          break;
        }
      }
    }
    // --- Fall: first > lvl90, then <= lvl10 ---
    let found90 = -1;
    for (let i = 1; i < d.length; i++) {
      if (found90 < 0) {
        if (d[i-1] >= lvl90 && d[i] < lvl90) found90 = crossAt(i, lvl90);
      } else {
        if (d[i-1] > lvl10 && d[i] <= lvl10) {
          fall = (crossAt(i, lvl10) - found90) * dtNs;
          break;
        }
      }
    }
    return { rise, fall };
  }

  // Compute a math channel on two aligned samples arrays. Returns a new
  // Float64Array (or null) matching the shorter of the two.
  function computeMath(op, a, b) {
    if (op === 'none') return null;
    const na = a ? a.length : 0;
    const nb = b ? b.length : 0;
    if (op === 'inva' && !a) return null;
    if (op === 'invb' && !b) return null;
    if ((op === 'add' || op === 'sub' || op === 'mul') && (!a || !b)) return null;
    const n = op === 'inva' ? na : op === 'invb' ? nb : Math.min(na, nb);
    if (n <= 0) return null;
    const out = new Float64Array(n);
    if (op === 'add')      for (let i = 0; i < n; i++) out[i] = a[i] + b[i];
    else if (op === 'sub') for (let i = 0; i < n; i++) out[i] = a[i] - b[i];
    else if (op === 'mul') for (let i = 0; i < n; i++) out[i] = (a[i] * b[i]) / 1000; // mV*mV/1000 ≈ V·mV
    else if (op === 'inva')for (let i = 0; i < n; i++) out[i] = -a[i];
    else if (op === 'invb')for (let i = 0; i < n; i++) out[i] = -b[i];
    return out;
  }

  $: mathTrace = computeMath(mathOp, visible.samplesA, visible.samplesB);

  $: {
    const v = visible;
    const dt = viewDtNs;
    measA = (v && v.samplesA && chAEnabled) ? computeMeas(v.samplesA, dt) : null;
    measB = (v && v.samplesB && chBEnabled) ? computeMeas(v.samplesB, dt) : null;
    measM = (mathTrace) ? computeMeas(mathTrace, dt) : null;
  }

  // ---- Rolling statistics (min/max/avg of each measurement over last N frames).
  const STATS_WINDOW = 50;
  let statsEnabled = false;
  const STATS_FIELDS = ['vpp', 'mean', 'rms', 'freqHz', 'duty'];
  function makeEmptyStats() {
    const out = {};
    for (const k of STATS_FIELDS) out[k] = [];
    return out;
  }
  let statsA = makeEmptyStats();
  let statsB = makeEmptyStats();
  function pushStat(bag, m) {
    if (!m) return;
    for (const k of STATS_FIELDS) {
      const v = m[k];
      if (typeof v !== 'number' || !isFinite(v)) continue;
      bag[k].push(v);
      if (bag[k].length > STATS_WINDOW) bag[k].shift();
    }
  }
  function statAgg(arr) {
    if (!arr || arr.length === 0) return null;
    let mn = arr[0], mx = arr[0], s = 0;
    for (const v of arr) { if (v < mn) mn = v; if (v > mx) mx = v; s += v; }
    return { min: mn, max: mx, avg: s / arr.length, n: arr.length };
  }
  // Accumulate only when stats are enabled — otherwise feed in zero-ops.
  $: if (statsEnabled) { pushStat(statsA, measA); pushStat(statsB, measB); }
  $: statsADisplay = statsEnabled ? {
    vpp: statAgg(statsA.vpp), mean: statAgg(statsA.mean),
    rms: statAgg(statsA.rms), freqHz: statAgg(statsA.freqHz),
    duty: statAgg(statsA.duty),
  } : null;
  $: statsBDisplay = statsEnabled ? {
    vpp: statAgg(statsB.vpp), mean: statAgg(statsB.mean),
    rms: statAgg(statsB.rms), freqHz: statAgg(statsB.freqHz),
    duty: statAgg(statsB.duty),
  } : null;
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
      <!-- Channel A -->
      <details class="panel" open>
        <summary>
          <span class="ch-dot" style="background: {chAEnabled ? '#00ff88' : '#333'}"></span>
          Channel A
        </summary>
        <div class="panel-content">
          <div class="form-row">
            <label>
              <span class="toggle-label">
                <input type="checkbox" bind:checked={chAEnabled} on:change={updateChannelA}>
                Enabled
              </span>
            </label>
          </div>
          <div class="form-row">
            <label>Coupling</label>
            <select bind:value={chACoupling} on:change={updateChannelA}>
              <option value="DC">DC</option>
              <option value="AC">AC</option>
            </select>
          </div>
          <div class="form-row">
            <label>Range</label>
            <select bind:value={chARange} on:change={updateChannelA}>
              {#each RANGES as r}
                <option value={r}>{r}</option>
              {/each}
            </select>
          </div>
          <div class="form-row">
            <label>Offset mV</label>
            <input type="number" bind:value={offsetMvA} step="50" title="Vertical offset (mV, added to trace)">
          </div>
          <div class="form-row">
            <label>V/div mV</label>
            <input type="number" bind:value={vdivMvA} min="0" step="10" title="0 = auto from range">
          </div>
        </div>
      </details>

      <!-- Channel B -->
      <details class="panel">
        <summary>
          <span class="ch-dot" style="background: {chBEnabled ? '#ffd700' : '#333'}"></span>
          Channel B
        </summary>
        <div class="panel-content">
          <div class="form-row">
            <label>
              <span class="toggle-label">
                <input type="checkbox" bind:checked={chBEnabled} on:change={updateChannelB}>
                Enabled
              </span>
            </label>
          </div>
          <div class="form-row">
            <label>Coupling</label>
            <select bind:value={chBCoupling} on:change={updateChannelB}>
              <option value="DC">DC</option>
              <option value="AC">AC</option>
            </select>
          </div>
          <div class="form-row">
            <label>Range</label>
            <select bind:value={chBRange} on:change={updateChannelB}>
              {#each RANGES as r}
                <option value={r}>{r}</option>
              {/each}
            </select>
          </div>
          <div class="form-row">
            <label>Offset mV</label>
            <input type="number" bind:value={offsetMvB} step="50" title="Vertical offset (mV)">
          </div>
          <div class="form-row">
            <label>V/div mV</label>
            <input type="number" bind:value={vdivMvB} min="0" step="10" title="0 = auto from range">
          </div>
        </div>
      </details>

      <!-- Timebase -->
      <details class="panel" open>
        <summary>Timebase</summary>
        <div class="panel-content">
          <div class="form-row">
            <label>TB</label>
            <select bind:value={timebase} on:change={updateTimebase}>
              {#each timebases as tb, i}
                <option value={i}>{i} - {tb.label}</option>
              {/each}
              {#if timebases.length === 0}
                {#each Array(11) as _, i}
                  <option value={i}>{i}</option>
                {/each}
              {/if}
            </select>
          </div>
          {#if tbLabel}
            <div class="form-row">
              <label>Interval</label>
              <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">{tbLabel}/sample</span>
            </div>
          {/if}
          <div class="form-row">
            <label>Samples</label>
            <input type="number" bind:value={samples}
                   on:change={updateSamples}
                   min="64" max={maxSamples} step="64"
                   disabled={!connected}>
          </div>
          <div class="form-row">
            <label>Max</label>
            <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
              {maxSamples} {chAEnabled && chBEnabled ? '(dual)' : '(single)'}
            </span>
          </div>
        </div>
      </details>

      <!-- Trigger -->
      <details class="panel">
        <summary>Trigger</summary>
        <div class="panel-content">
          <div class="form-row">
            <label>
              <span class="toggle-label">
                <input type="checkbox" bind:checked={triggerEnabled} on:change={updateTrigger}>
                Enabled
              </span>
            </label>
          </div>
          <div class="form-row">
            <label>Source</label>
            <select bind:value={triggerSource} on:change={updateTrigger} disabled={!triggerEnabled}>
              <option value="A">Channel A</option>
              <option value="B">Channel B</option>
            </select>
          </div>
          <div class="form-row">
            <label>Level</label>
            <input type="number" bind:value={triggerThreshold} on:change={updateTrigger}
                   disabled={!triggerEnabled} step="100" placeholder="mV">
          </div>
          <div class="form-row">
            <label>Edge</label>
            <select bind:value={triggerDirection} on:change={updateTrigger} disabled={!triggerEnabled}>
              <option value="rising">Rising</option>
              <option value="falling">Falling</option>
            </select>
          </div>
          <div class="form-row">
            <label>Auto ms</label>
            <input type="number" bind:value={triggerAutoMs} on:change={updateTrigger}
                   disabled={!triggerEnabled} min="0" step="100">
          </div>
        </div>
      </details>

      <!-- Signal Generator -->
      <details class="panel">
        <summary>Signal Generator</summary>
        <div class="panel-content">
          <div class="form-row">
            <label>Wave</label>
            <select bind:value={siggenWave}>
              {#each WAVE_TYPES as w}
                <option value={w}>{w}</option>
              {/each}
            </select>
          </div>
          <div class="form-row">
            <label>Freq Hz</label>
            <input type="number" bind:value={siggenFreq} min="1" max="100000" step="100">
          </div>
          <div class="form-row">
            <label>Ampl mVpp</label>
            <input type="number" bind:value={siggenAmpMv} min="50" max="2000" step="50">
          </div>
          <div class="form-row">
            <label>Offset mV</label>
            <input type="number" bind:value={siggenOffsetMv} min="-2000" max="2000" step="50">
          </div>
          {#if siggenWave === 'square'}
            <div class="form-row">
              <label>Duty %</label>
              <input type="number" bind:value={siggenDuty} min="1" max="99" step="1">
            </div>
          {/if}
          <div class="form-row">
            <label>
              <span class="toggle-label">
                <input type="checkbox" bind:checked={siggenSweep}>
                Sweep
              </span>
            </label>
          </div>
          {#if siggenSweep}
            <div class="form-row">
              <label>Stop Hz</label>
              <input type="number" bind:value={siggenStopFreq} min="1" max="100000" step="100">
            </div>
            <div class="form-row">
              <label>Step Hz</label>
              <input type="number" bind:value={siggenSweepInc} min="1" max="10000" step="10">
            </div>
            <div class="form-row">
              <label>Dwell s</label>
              <input type="number" bind:value={siggenSweepDwell} min="0.001" max="0.35" step="0.01">
            </div>
          {/if}
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={handleSiggenOn} disabled={!connected}>
              {siggenEnabled ? 'Update' : 'Enable'}
            </button>
            <button class="btn btn-danger" on:click={handleSiggenOff}
                    disabled={!connected || !siggenEnabled}>
              Disable
            </button>
          </div>
        </div>
      </details>

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
      <details class="panel">
        <summary>Calibration</summary>
        <div class="panel-content">
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={() => calModalOpen = true}
                    disabled={!connected}>
              Open calibration editor
            </button>
          </div>
          <div class="form-row" style="margin-top:8px;">
            <span class="meas-label">Or quick actions:</span>
          </div>
          <div class="form-row">
            <span class="meas-label">Short your inputs (or leave them floating) to 0 V, then click:</span>
          </div>
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={handleCalibrateDCOffset}
                    disabled={!connected || isStreaming}>
              Auto-calibrate DC offset
            </button>
          </div>
          <div class="form-row" style="margin-top:8px;">
            <span class="meas-label">Manual per-range:</span>
          </div>
          <div class="form-row">
            <label>Range</label>
            <select bind:value={calRange}>
              {#each RANGES as r}<option value={r}>{r}</option>{/each}
            </select>
          </div>
          <div class="form-row">
            <label>Offset mV</label>
            <input type="number" bind:value={calOffsetMv} step="1">
          </div>
          <div class="form-row">
            <label>Gain</label>
            <input type="number" bind:value={calGain} step="0.01" min="0.1" max="5">
          </div>
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={handleSetRangeCal}
                    disabled={!connected}>Apply to range</button>
          </div>
        </div>
      </details>

      <!-- Presets -->
      <details class="panel">
        <summary>Presets</summary>
        <div class="panel-content">
          <div class="form-row">
            <label>Name</label>
            <input type="text" bind:value={presetName} placeholder="e.g. 1kHz sine test">
          </div>
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={handlePresetSave}>Save current</button>
          </div>
          {#if Object.keys(presets).length > 0}
            <div class="form-row">
              <label>Load</label>
              <select bind:value={selectedPreset}>
                <option value="">—</option>
                {#each Object.keys(presets) as name}
                  <option value={name}>{name}</option>
                {/each}
              </select>
            </div>
            <div class="panel-actions">
              <button class="btn btn-primary" on:click={handlePresetLoad}
                      disabled={!selectedPreset}>Load</button>
              <button class="btn btn-danger" on:click={handlePresetDelete}
                      disabled={!selectedPreset}>Delete</button>
            </div>
          {/if}
        </div>
      </details>

      <!-- Diagnostics -->
      <details class="panel">
        <summary>Diagnostics</summary>
        <div class="panel-content">
          <div class="panel-actions">
            <button class="btn btn-primary" on:click={handleCaptureRaw}
                    disabled={!connected || isStreaming}>
              Capture Raw
            </button>
          </div>
          {#if rawPreview}
            <div class="form-row">
              <label>Bytes</label>
              <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
                {rawPreview.numBytes}
              </span>
            </div>
            <div class="form-row">
              <label>Samples</label>
              <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
                {rawPreview.numSamples}
              </span>
            </div>
            <div class="form-row">
              <label>Timebase</label>
              <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
                {rawPreview.timebase} ({rawPreview.timebaseNs} ns)
              </span>
            </div>
            <div class="form-row">
              <label>Dual</label>
              <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
                {rawPreview.dual ? 'yes (B,A,B,A tail)' : 'no (CH A only)'}
              </span>
            </div>
            {#if rawPreview.bytes && rawPreview.bytes.length > 0}
              <div class="form-row" style="flex-direction: column; align-items: flex-start; gap: 4px;">
                <label>Head (hex)</label>
                <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 11px; word-break: break-all; line-height: 1.4;">
                  {rawPreview.bytes.slice(0, 32).map(b => b.toString(16).padStart(2, '0')).join(' ')}
                </span>
              </div>
            {/if}
          {/if}
        </div>
      </details>
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
      <div class="display-controls">
        <span class="ctl-label">Time/div</span>

        <div class="ctl-dropdown" bind:this={tdButton}>
          <button type="button" class="ctl-dropdown-trigger"
                  on:click={tdToggle}
                  aria-haspopup="listbox" aria-expanded={tdOpen}>
            {tdLabel} <span class="caret">▾</span>
          </button>
          {#if tdOpen}
            <ul class="ctl-dropdown-menu" role="listbox">
              {#each TIME_DIV_PRESETS as p}
                <li class="ctl-dropdown-item"
                    class:selected={p.ns === timePerDivNs}
                    role="option"
                    aria-selected={p.ns === timePerDivNs}
                    on:click={(ev) => tdPick(p.ns, ev)}>
                  {p.label}
                </li>
              {/each}
            </ul>
          {/if}
        </div>

        <button class="ctl-btn" on:click={() => shiftDiv(-1)}
                disabled={!offsetActive} title="Previous division">◁</button>
        <button class="ctl-btn" on:click={() => shiftDiv(1)}
                disabled={!offsetActive} title="Next division">▷</button>

        {#if isStreaming}
          <button class="ctl-btn" class:active={streamPaused}
                  on:click={togglePause}
                  title={streamPaused ? 'Resume live roll' : 'Pause streaming view'}>
            {streamPaused ? '▶ Resume' : '⏸ Pause'}
          </button>
        {/if}

        <button class="ctl-btn" on:click={fitView} title="Fit entire buffer">Fit</button>

        <div class="dual-range" class:disabled={!dualRangeActive}
             title="Drag either handle to define the visible window (start / end)">
          <div class="dual-range-track"></div>
          <div class="dual-range-fill"
               style="left:{windowStartPct}%;right:{100 - windowEndPct}%"></div>
          <input type="range" class="dual-range-input dual-range-min"
                 min="0" max="100" step="0.1"
                 value={windowStartPct}
                 on:input={onRangeMinInput}
                 disabled={!dualRangeActive}
                 aria-label="Window start">
          <input type="range" class="dual-range-input dual-range-max"
                 min="0" max="100" step="0.1"
                 value={windowEndPct}
                 on:input={onRangeMaxInput}
                 disabled={!dualRangeActive}
                 aria-label="Window end">
        </div>
        <span class="ctl-spanlabel">
          {#if visible.spanNs > 0}
            {fmtTime(visible.spanNs)} span
          {:else}
            —
          {/if}
        </span>
      </div>

      <!-- Analysis controls: math, cursors, XY, persistence, averaging, FFT -->
      <div class="display-controls">
        <span class="ctl-label">Math</span>
        <select class="select-inline" bind:value={mathOp}>
          <option value="none">off</option>
          <option value="add">A + B</option>
          <option value="sub">A − B</option>
          <option value="mul">A × B / 1000</option>
          <option value="inva">−A</option>
          <option value="invb">−B</option>
        </select>

        <button class="ctl-btn" class:active={cursorsOn}
                on:click={() => cursorsOn = !cursorsOn}
                title="Vertical cursors (Δt / 1÷Δt)">Δt</button>

        <button class="ctl-btn" class:active={yCursorsOn}
                on:click={() => yCursorsOn = !yCursorsOn}
                title="Horizontal cursors (ΔV)">ΔV</button>

        <button class="ctl-btn" class:active={xyMode}
                on:click={() => xyMode = !xyMode}
                title="X-Y Lissajous mode (A vs B)">XY</button>

        <button class="ctl-btn" class:active={persistenceOn}
                on:click={() => persistenceOn = !persistenceOn}
                title="Phosphor-like overlay">Persist</button>

        <span class="ctl-label">Avg</span>
        <select class="select-inline" bind:value={avgN}>
          <option value={1}>1</option>
          <option value={4}>4</option>
          <option value={16}>16</option>
          <option value={64}>64</option>
        </select>

        <button class="ctl-btn" class:active={fftOn}
                on:click={() => fftOn = !fftOn}
                title="Show FFT spectrum of CH A">FFT</button>

        <button class="ctl-btn" class:active={statsEnabled}
                on:click={() => { statsEnabled = !statsEnabled; if (!statsEnabled) resetStats(); }}
                title="Accumulate min/max/avg of each measurement over last 50 captures">Stats</button>

        <div class="meas-picker-wrap">
          <button class="ctl-btn" class:active={measMenuOpen}
                  on:click={() => measMenuOpen = !measMenuOpen}
                  title="Choose which measurements appear in the bottom bar">Meas ▾</button>
          {#if measMenuOpen}
            <div class="meas-picker" on:click|stopPropagation>
              <div class="meas-picker-title">Amplitude</div>
              {#each MEAS_CATALOG.filter(m => m.group === 'amp') as m}
                <label class="meas-picker-row">
                  <input type="checkbox" checked={measKeys.has(m.key)}
                         on:change={() => toggleMeasKey(m.key)} />
                  <span>{m.label}</span>
                </label>
              {/each}
              <div class="meas-picker-title">Time</div>
              {#each MEAS_CATALOG.filter(m => m.group === 'time') as m}
                <label class="meas-picker-row">
                  <input type="checkbox" checked={measKeys.has(m.key)}
                         on:change={() => toggleMeasKey(m.key)} />
                  <span>{m.label}</span>
                </label>
              {/each}
              <div class="meas-picker-actions">
                <button class="ctl-btn" on:click={() => measKeys = new Set(MEAS_CATALOG.map(m => m.key))}>All</button>
                <button class="ctl-btn" on:click={() => measKeys = new Set(MEAS_DEFAULT)}>Default</button>
                <button class="ctl-btn" on:click={() => measKeys = new Set()}>None</button>
              </div>
            </div>
          {/if}
        </div>

        <button class="ctl-btn" on:click={handleExportCSV}
                disabled={!visible.samplesA || visible.samplesA.length === 0}
                title="Export visible slice as CSV">CSV</button>

        <button class="ctl-btn" on:click={handleExportPNG}
                title="Save scope screenshot">PNG</button>

        <button class="ctl-btn" on:click={handleAutoSetup}
                disabled={!connected || isStreaming}
                title="Analyse signal and auto-configure range + timebase + trigger">Auto</button>
      </div>

      <!-- Measurements -->
      <div class="measurements-bar">
        {#if measA}
          <div class="meas-group">
            <span class="meas-ch-a">CH A</span>
            {#each MEAS_CATALOG as m}
              {#if measKeys.has(m.key)}
                <span class="meas-label">{m.label}:</span>
                <span class="meas-value">{fmtMeas(m.fmt, measA[m.key])}</span>
              {/if}
            {/each}
            {#if statsADisplay && statsADisplay.vpp && measKeys.has('vpp')}
              <span class="meas-stats">(Vpp μ={fmtMv(statsADisplay.vpp.avg)} σ=[{fmtMv(statsADisplay.vpp.min)}..{fmtMv(statsADisplay.vpp.max)}] n={statsADisplay.vpp.n})</span>
            {/if}
          </div>
        {/if}
        {#if measB}
          <div class="meas-group">
            <span class="meas-ch-b">CH B</span>
            {#each MEAS_CATALOG as m}
              {#if measKeys.has(m.key)}
                <span class="meas-label">{m.label}:</span>
                <span class="meas-value">{fmtMeas(m.fmt, measB[m.key])}</span>
              {/if}
            {/each}
            {#if statsBDisplay && statsBDisplay.vpp && measKeys.has('vpp')}
              <span class="meas-stats">(Vpp μ={fmtMv(statsBDisplay.vpp.avg)} σ=[{fmtMv(statsBDisplay.vpp.min)}..{fmtMv(statsBDisplay.vpp.max)}] n={statsBDisplay.vpp.n})</span>
            {/if}
          </div>
        {/if}
        {#if measM}
          <div class="meas-group">
            <span class="meas-ch-m">MATH</span>
            {#each MEAS_CATALOG as m}
              {#if measKeys.has(m.key)}
                <span class="meas-label">{m.label}:</span>
                <span class="meas-value">{fmtMeas(m.fmt, measM[m.key])}</span>
              {/if}
            {/each}
          </div>
        {/if}
        {#if !measA && !measB}
          <span class="meas-label">No data</span>
        {/if}
      </div>
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

  <!-- Status bar -->
  <div class="status-bar">
    <span>
      <span class="status-dot" class:connected class:disconnected={!connected}></span>
      {#if connected}Connected{:else}Disconnected{/if}
    </span>

    {#if serial}
      <span>Serial: {serial}</span>
    {/if}
    {#if calDate}
      <span>Cal: {calDate}</span>
    {/if}

    {#if isStreaming}
      <span>
        <span class="status-dot streaming"></span>
        Streaming
      </span>
      {#if streamStats}
        <span>{fmtRate(streamStats.samplesPerSec)}</span>
        <span>{fmtCount(streamStats.totalSamples)} samples</span>
        <span>{streamStats.lastBlockMs.toFixed(1)} ms/block</span>
      {/if}
    {/if}

    {#if waveformData && !isStreaming}
      <span>{waveformData.numSamples} samples</span>
      <span>{timebases[waveformData.timebase] ? timebases[waveformData.timebase].label : ''}/sample</span>
    {/if}
  </div>
</div>

{#if errorMsg}
  <div class="error-toast">{errorMsg}</div>
{/if}
