<script>
  let {
    label     = 'Channel A',
    dotColor  = '#00ff88',
    ranges    = [],
    panelOpen = false,

    enabled  = $bindable(false),
    coupling = $bindable('DC'),
    range    = $bindable('5V'),
    offsetMv = $bindable(0),
    vdivMv   = $bindable(0),

    onChange = () => {},
  } = $props();
</script>

<details class="panel" open={panelOpen}>
  <summary>
    <span class="ch-dot" style="background: {enabled ? dotColor : '#333'}"></span>
    {label}
  </summary>
  <div class="panel-content">
    <div class="form-row">
      <label>
        <span class="toggle-label">
          <input type="checkbox" bind:checked={enabled} onchange={onChange}>
          Enabled
        </span>
      </label>
    </div>
    <div class="form-row">
      <label>Coupling</label>
      <select bind:value={coupling} onchange={onChange}>
        <option value="DC">DC</option>
        <option value="AC">AC</option>
      </select>
    </div>
    <div class="form-row">
      <label>Range</label>
      <select bind:value={range} onchange={onChange}>
        {#each ranges as r}
          <option value={r}>{r}</option>
        {/each}
      </select>
    </div>
    <div class="form-row">
      <label>Offset mV</label>
      <input type="number" bind:value={offsetMv} step="50"
             title="Vertical offset (mV, added to trace)">
    </div>
    <div class="form-row">
      <label>V/div mV</label>
      <input type="number" bind:value={vdivMv} min="0" step="10"
             title="0 = auto from range">
    </div>
  </div>
</details>
