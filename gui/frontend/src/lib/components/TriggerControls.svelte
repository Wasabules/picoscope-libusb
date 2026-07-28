<script>
  let {
    enabled   = $bindable(false),
    source    = $bindable('A'),
    threshold = $bindable(0),
    direction = $bindable('rising'),
    autoMs    = $bindable(0),

    onChange = () => {},
  } = $props();
</script>

<details class="panel">
  <summary>Trigger</summary>
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
      <label>Source</label>
      <select bind:value={source} onchange={onChange} disabled={!enabled}>
        <option value="A">Channel A</option>
        <option value="B">Channel B</option>
      </select>
    </div>
    <div class="form-row">
      <label>Level</label>
      <input type="number" bind:value={threshold} onchange={onChange}
             disabled={!enabled} step="100" placeholder="mV">
    </div>
    <div class="form-row">
      <label>Edge</label>
      <select bind:value={direction} onchange={onChange} disabled={!enabled}>
        <option value="rising">Rising</option>
        <option value="falling">Falling</option>
      </select>
    </div>
    <div class="form-row">
      <label>Auto ms</label>
      <input type="number" bind:value={autoMs} onchange={onChange}
             disabled={!enabled} min="0" step="100">
    </div>
  </div>
</details>
