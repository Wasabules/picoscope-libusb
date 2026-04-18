<script>
  import { createEventDispatcher, onMount } from 'svelte';
  import {
    GetAllCalibration, ApplyCalibration,
    SetChannelA, SetChannelB, CaptureBlock
  } from '../../wailsjs/go/main/App.js';

  export let open = false;

  const dispatch = createEventDispatcher();

  const RANGES = ['50mV', '100mV', '200mV', '500mV', '1V', '2V', '5V', '10V', '20V'];
  const RANGE_MV = {
    '50mV': 50, '100mV': 100, '200mV': 200, '500mV': 500,
    '1V': 1000, '2V': 2000, '5V': 5000, '10V': 10000, '20V': 20000
  };

  // Per-range: list of {expected: mv, measured: mv} points.
  // Auto-bootstrapped with one empty row each.
  let points = {};
  for (const r of RANGES) points[r] = [{ expected: null, measured: null }];

  // Current committed (offset, gain) per range (from the driver).
  let current = {};
  for (const r of RANGES) current[r] = { offset_mv: 0, gain: 1 };

  // Active row being measured — for visual feedback.
  let busyRange = null;
  let busyIdx = -1;
  let msg = '';

  async function refreshFromDriver() {
    try {
      const rows = await GetAllCalibration();
      for (const e of rows) {
        current[e.range] = { offset_mv: e.offset_mv, gain: e.gain };
      }
      current = current;
    } catch (e) { msg = 'Read failed: ' + e; }
  }

  onMount(() => {
    if (open) refreshFromDriver();
  });
  $: if (open) refreshFromDriver();

  function addPoint(range) {
    points[range] = [...points[range], { expected: null, measured: null }];
  }
  function removePoint(range, i) {
    points[range] = points[range].filter((_, k) => k !== i);
    if (points[range].length === 0) points[range] = [{ expected: null, measured: null }];
  }

  async function measure(range, i) {
    busyRange = range; busyIdx = i; msg = '';
    try {
      await SetChannelA(true, 'DC', range);
      await SetChannelB(false, 'DC', '5V');
      await new Promise(r => setTimeout(r, 300));
      // Two captures: first to settle, second to keep.
      await CaptureBlock();
      const data = await CaptureBlock();
      if (!data || !data.channelA || data.channelA.length === 0) {
        throw new Error('capture returned no samples');
      }
      let sum = 0;
      for (const v of data.channelA) sum += v;
      const mean = sum / data.channelA.length;
      points[range][i].measured = +mean.toFixed(2);
      points = points;  // trigger Svelte
    } catch (e) {
      msg = 'Measure failed: ' + e;
    }
    busyRange = null; busyIdx = -1;
  }

  // Simple linear regression: fit `measured = a × expected + b`.
  // The driver applies `corrected = (raw − offset) × gain`, so:
  //   corrected = expected  →  raw = measured
  //   expected = (measured − offset) × gain
  //   ⇒ gain = 1 / a and offset = −b / a  (if fit was expected on Y)
  // We fit measured(Y) against expected(X), so a=slope, b=intercept:
  //   measured = slope × expected + intercept
  //   expected = (measured − intercept) / slope
  //            = (raw − intercept) × (1/slope)
  //   ⇒ offset_mv = intercept,  gain = 1 / slope
  function fitLinear(pts) {
    const xs = [], ys = [];
    for (const p of pts) {
      if (p.expected != null && p.measured != null && isFinite(p.expected) && isFinite(p.measured)) {
        xs.push(+p.expected); ys.push(+p.measured);
      }
    }
    const n = xs.length;
    if (n === 0) return null;
    if (n === 1) {
      // Single point: force slope=1, intercept = measured − expected.
      return { offset_mv: ys[0] - xs[0], gain: 1, n: 1 };
    }
    let sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (let i = 0; i < n; i++) {
      sx += xs[i]; sy += ys[i]; sxx += xs[i]*xs[i]; sxy += xs[i]*ys[i];
    }
    const denom = n * sxx - sx * sx;
    if (Math.abs(denom) < 1e-9) return { offset_mv: sy/n - sx/n, gain: 1, n };
    const slope = (n * sxy - sx * sy) / denom;
    const intercept = (sy - slope * sx) / n;
    if (slope <= 0) return null;
    return { offset_mv: intercept, gain: 1 / slope, n };
  }

  $: fits = Object.fromEntries(RANGES.map(r => [r, fitLinear(points[r])]));

  async function applyAll() {
    const payload = [];
    for (const r of RANGES) {
      const f = fits[r];
      if (!f) continue;
      payload.push({ range: r, offset_mv: f.offset_mv, gain: f.gain });
    }
    if (payload.length === 0) { msg = 'No valid fits to apply'; return; }
    try {
      await ApplyCalibration(payload);
      msg = 'Applied ' + payload.length + ' range(s)';
      await refreshFromDriver();
    } catch (e) { msg = 'Apply failed: ' + e; }
  }

  function exportJSON() {
    const data = {
      version: 1,
      created: new Date().toISOString(),
      // Include both the fitted (offset, gain) AND the raw points for
      // reproducibility / future re-analysis.
      fits: Object.fromEntries(RANGES.map(r => [r, fits[r]])),
      points: points,
    };
    const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'picoscope_calibration.json';
    a.click();
    URL.revokeObjectURL(url);
    msg = 'Exported calibration JSON';
  }

  async function exportToClipboard() {
    const data = {
      version: 1, created: new Date().toISOString(),
      fits: Object.fromEntries(RANGES.map(r => [r, fits[r]])),
      points: points,
    };
    try {
      await navigator.clipboard.writeText(JSON.stringify(data, null, 2));
      msg = 'Calibration JSON copied to clipboard';
    } catch (e) { msg = 'Clipboard failed: ' + e; }
  }

  let fileInput;
  function pickImport() { fileInput.click(); }
  async function onImport(ev) {
    const file = ev.target.files[0];
    if (!file) return;
    try {
      const text = await file.text();
      const data = JSON.parse(text);
      if (data.points) {
        for (const r of RANGES) {
          if (data.points[r] && Array.isArray(data.points[r]) && data.points[r].length > 0) {
            points[r] = data.points[r].map(p => ({
              expected: p.expected, measured: p.measured
            }));
          }
        }
        points = points;
        msg = 'Imported ' + file.name + ' — click Apply to push to driver';
      } else if (data.fits) {
        // Only fits available — push directly
        const payload = [];
        for (const r of RANGES) {
          const f = data.fits[r];
          if (f && f.gain) payload.push({ range: r, offset_mv: f.offset_mv, gain: f.gain });
        }
        await ApplyCalibration(payload);
        await refreshFromDriver();
        msg = 'Imported + applied (fits only)';
      } else {
        msg = 'JSON missing `points` and `fits`';
      }
    } catch (e) { msg = 'Import failed: ' + e; }
    ev.target.value = '';
  }

  function close() { dispatch('close'); }
</script>

{#if open}
<div class="modal-backdrop" on:click={close}>
  <div class="modal" on:click|stopPropagation>
    <div class="modal-header">
      <h2>Calibration</h2>
      <button class="icon-btn" on:click={close} title="Close">✕</button>
    </div>

    <div class="modal-body">
      <p class="hint">
        For each range, enter one or more (expected, measured) pairs. Expected = true
        voltage from your DMM. Measured = what the scope reads — click "Measure"
        to capture it live with the current driver. A linear fit produces
        <code>(offset, gain)</code>; "Apply" pushes it to the driver.
      </p>

      <div class="toolbar">
        <button class="btn" on:click={exportJSON}>⬇ Export JSON</button>
        <button class="btn" on:click={exportToClipboard}>⎘ Copy JSON</button>
        <button class="btn" on:click={pickImport}>⬆ Import JSON</button>
        <button class="btn btn-primary" on:click={applyAll}>✓ Apply to driver</button>
        <input bind:this={fileInput} type="file" accept=".json" on:change={onImport}
               style="display:none">
      </div>

      {#if msg}
        <div class="status">{msg}</div>
      {/if}

      <table class="cal-table">
        <thead>
          <tr>
            <th>Range</th>
            <th>Current (offset mV / gain)</th>
            <th>Points (Expected mV / Measured mV)</th>
            <th>Fit (offset / gain)</th>
          </tr>
        </thead>
        <tbody>
          {#each RANGES as r}
            <tr>
              <td class="range">{r}</td>
              <td class="current">
                <span>{current[r].offset_mv.toFixed(2)}</span>
                <span>/</span>
                <span>{current[r].gain.toFixed(4)}</span>
              </td>
              <td class="points">
                {#each points[r] as p, i}
                  <div class="point-row">
                    <input type="number" step="any"
                           placeholder="Expected mV"
                           bind:value={p.expected}>
                    <span>→</span>
                    <input type="number" step="any"
                           placeholder="Measured mV"
                           bind:value={p.measured}>
                    <button class="mini-btn"
                            class:busy={busyRange === r && busyIdx === i}
                            on:click={() => measure(r, i)}
                            title="Capture CH A mean at this range">
                      {busyRange === r && busyIdx === i ? '…' : '⏺'}
                    </button>
                    {#if points[r].length > 1}
                      <button class="mini-btn" on:click={() => removePoint(r, i)}
                              title="Remove point">✕</button>
                    {/if}
                  </div>
                {/each}
                <button class="mini-btn" on:click={() => addPoint(r)}>+ add point</button>
              </td>
              <td class="fit">
                {#if fits[r]}
                  <span>{fits[r].offset_mv.toFixed(2)} mV</span><br>
                  <span>× {fits[r].gain.toFixed(4)}</span><br>
                  <span class="n">n={fits[r].n}</span>
                {:else}
                  <span class="n">—</span>
                {/if}
              </td>
            </tr>
          {/each}
        </tbody>
      </table>
    </div>
  </div>
</div>
{/if}

<style>
  .modal-backdrop {
    position: fixed; inset: 0; background: rgba(0,0,0,0.7);
    display: flex; align-items: center; justify-content: center;
    z-index: 1000;
  }
  .modal {
    background: #0a0e17; color: #e6edf3;
    border: 1px solid #2a3a4a; border-radius: 6px;
    width: min(1100px, 95vw); max-height: 90vh;
    display: flex; flex-direction: column;
    box-shadow: 0 10px 40px rgba(0,0,0,0.8);
  }
  .modal-header {
    display: flex; align-items: center; justify-content: space-between;
    padding: 12px 18px; border-bottom: 1px solid #1e2d3d;
  }
  .modal-header h2 { margin: 0; font-size: 16px; }
  .icon-btn {
    background: none; border: 0; color: #8899aa; font-size: 18px;
    cursor: pointer; padding: 4px 8px;
  }
  .icon-btn:hover { color: #fff; }

  .modal-body {
    padding: 14px 18px; overflow-y: auto; flex: 1;
  }
  .hint { color: #8899aa; font-size: 12px; margin: 0 0 10px; line-height: 1.5; }
  .hint code { background: #1c2333; padding: 1px 5px; border-radius: 3px; }

  .toolbar {
    display: flex; gap: 8px; margin-bottom: 10px;
  }
  .btn {
    background: #1c2333; border: 1px solid #2a3a4a; color: #e6edf3;
    padding: 5px 10px; border-radius: 4px; cursor: pointer; font-size: 12px;
  }
  .btn:hover { background: #2a3a4a; }
  .btn-primary { background: #2a4055; border-color: #3a5a75; }
  .btn-primary:hover { background: #3a5a75; }

  .status {
    background: #1c2333; padding: 6px 10px; margin-bottom: 10px;
    border-left: 3px solid #66ddff; font-size: 12px;
  }

  .cal-table {
    width: 100%; border-collapse: collapse; font-size: 12px;
  }
  .cal-table th, .cal-table td {
    padding: 6px 8px; border-bottom: 1px solid #1e2d3d;
    text-align: left; vertical-align: top;
  }
  .cal-table th {
    background: #1c2333; color: #8899aa; font-weight: normal;
  }
  .cal-table td.range { font-weight: bold; color: #00ff88; }
  .cal-table td.current span { font-family: monospace; color: #8899aa; margin-right: 4px; }
  .cal-table td.fit { font-family: monospace; color: #66ddff; }
  .cal-table td.fit .n { color: #667788; font-size: 11px; }

  .point-row {
    display: flex; align-items: center; gap: 4px; margin-bottom: 3px;
  }
  .point-row input {
    width: 100px; padding: 2px 6px;
    background: #1c2333; border: 1px solid #2a3a4a;
    color: #e6edf3; border-radius: 3px; font-family: monospace; font-size: 12px;
  }
  .point-row span { color: #667788; }
  .mini-btn {
    background: #1c2333; border: 1px solid #2a3a4a; color: #e6edf3;
    padding: 2px 7px; border-radius: 3px; cursor: pointer; font-size: 11px;
  }
  .mini-btn:hover { background: #2a4055; }
  .mini-btn.busy { background: #3a5a75; }
</style>
