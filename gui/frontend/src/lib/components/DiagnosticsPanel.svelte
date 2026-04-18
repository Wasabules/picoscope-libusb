<script>
  export let connected = false;
  export let isStreaming = false;
  export let rawPreview = null;

  export let onCaptureRaw = () => {};
</script>

<details class="panel">
  <summary>Diagnostics</summary>
  <div class="panel-content">
    <div class="panel-actions">
      <button class="btn btn-primary" on:click={onCaptureRaw}
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
