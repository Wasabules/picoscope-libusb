<script>
  export let ranges = [];
  export let range = '5V';
  export let offsetMv = 0;
  export let gain = 1;
  export let connected = false;
  export let isStreaming = false;

  export let onOpenEditor = () => {};
  export let onAutoCalibrate = () => {};
  export let onApply = () => {};
</script>

<details class="panel">
  <summary>Calibration</summary>
  <div class="panel-content">
    <div class="panel-actions">
      <button class="btn btn-primary" on:click={onOpenEditor}
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
      <button class="btn btn-primary" on:click={onAutoCalibrate}
              disabled={!connected || isStreaming}>
        Auto-calibrate DC offset
      </button>
    </div>
    <div class="form-row" style="margin-top:8px;">
      <span class="meas-label">Manual per-range:</span>
    </div>
    <div class="form-row">
      <label>Range</label>
      <select bind:value={range}>
        {#each ranges as r}<option value={r}>{r}</option>{/each}
      </select>
    </div>
    <div class="form-row">
      <label>Offset mV</label>
      <input type="number" bind:value={offsetMv} step="1">
    </div>
    <div class="form-row">
      <label>Gain</label>
      <input type="number" bind:value={gain} step="0.01" min="0.1" max="5">
    </div>
    <div class="panel-actions">
      <button class="btn btn-primary" on:click={onApply}
              disabled={!connected}>Apply to range</button>
    </div>
  </div>
</details>
