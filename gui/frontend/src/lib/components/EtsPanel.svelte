<script>
  /**
   * Equivalent-time sampling.
   *
   * The ADC runs at 100 MS/s. On a repetitive signal the device can capture
   * many triggered blocks, each offset by a fraction of a sample period, and
   * interleave them — so the effective rate becomes a multiple of the real
   * one: 1 GS/s in fast mode, 2 GS/s in slow mode. It only works on a signal
   * that repeats and that the trigger can lock onto; a one-shot event will
   * produce noise.
   *
   * ETS reconstructs the phase of each block by interpolating the trigger
   * crossing, and that needs samples either side of it. Below timebase 3 there
   * are too few: measured on a 200 kHz sine, tb=0 never completes at all,
   * tb=1 is flaky and loses 60 % of the amplitude, tb=2 loses 30 %. The traces
   * stay smooth while shrinking, which is what averaging misaligned bins does
   * — so it fails quietly, hence the warning rather than a silent bad capture.
   */
  let {
    mode        = $bindable('off'),   // off | fast | slow
    interleaves = $bindable(0),       // 0 = mode default
    cycles      = $bindable(0),
    samples     = $bindable(500),

    intervalPs = 0,                   // reported by the driver once armed
    timebase   = 5,                   // current acquisition timebase
    connected  = false,
    busy       = false,
    lastCount  = 0,

    onApply   = () => {},
    onCapture = () => {},
  } = $props();

  const effRate = $derived(intervalPs > 0 ? 1e12 / intervalPs : 0);

  // Measured limits, see the comment above.
  const tbWarning = $derived(
    timebase <= 0 ? 'Timebase 0 never completes an ETS capture — use 3 or slower.'
    : timebase === 1 ? 'Timebase 1 is unreliable here and loses about 60 % of the amplitude — use 3 or slower.'
    : timebase === 2 ? 'Timebase 2 loses about 30 % of the amplitude — use 3 or slower for a faithful trace.'
    : '');

  function fmtRate(hz) {
    if (hz >= 1e9) return (hz / 1e9).toFixed(2) + ' GS/s';
    if (hz >= 1e6) return (hz / 1e6).toFixed(1) + ' MS/s';
    return hz.toFixed(0) + ' S/s';
  }
</script>

<details class="panel">
  <summary>ETS (equivalent-time)</summary>
  <div class="panel-content">
    <p class="hint">
      Rebuilds a repetitive waveform from many triggered captures, each offset
      by a fraction of a sample. Needs a stable trigger and a signal that
      repeats.
    </p>

    <div class="form-row">
      <label>Mode</label>
      <select bind:value={mode} onchange={onApply} disabled={!connected}>
        <option value="off">Off</option>
        <option value="fast">Fast (≈1 GS/s)</option>
        <option value="slow">Slow (≈2 GS/s)</option>
      </select>
    </div>

    {#if mode !== 'off'}
      <div class="form-row">
        <label>Interleaves</label>
        <input type="number" bind:value={interleaves} onchange={onApply}
               min="0" max="20" step="1" title="0 = mode default (10 fast / 20 slow)">
      </div>
      <div class="form-row">
        <label>Cycles</label>
        <input type="number" bind:value={cycles} onchange={onApply}
               min="0" max="32" step="1" title="0 = mode default. Captures averaged per bin.">
      </div>
      <div class="form-row">
        <label>Samples</label>
        <input type="number" bind:value={samples} min="10" max="8192" step="10">
      </div>

      {#if tbWarning}
        <p class="warn">{tbWarning}</p>
      {/if}

      {#if intervalPs > 0}
        <div class="form-row">
          <label>Effective</label>
          <span class="readout">{intervalPs} ps/sample — {fmtRate(effRate)}</span>
        </div>
      {/if}

      <div class="panel-actions">
        <button class="btn btn-primary" onclick={onCapture}
                disabled={!connected || busy}>
          {busy ? 'Capturing…' : 'ETS capture'}
        </button>
      </div>

      {#if lastCount > 0}
        <div class="form-row">
          <label>Last</label>
          <span class="readout">{lastCount} points</span>
        </div>
      {/if}
    {/if}
  </div>
</details>

<style>
  .hint {
    color: #8899aa; font-size: 11px; margin: 0 0 8px; line-height: 1.45;
  }
  .readout { color: #cfd8e3; font-family: monospace; font-size: 11px; }
  .warn {
    color: #ffcc66; background: #3a2f1a; border-left: 3px solid #ffaa33;
    font-size: 11px; line-height: 1.45; margin: 6px 0; padding: 6px 8px;
    border-radius: 3px;
  }
</style>
