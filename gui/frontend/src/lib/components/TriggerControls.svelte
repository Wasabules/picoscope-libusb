<script>
  export let enabled = false;
  export let source = 'A';
  export let threshold = 0;
  export let direction = 'rising';
  export let autoMs = 0;

  export let onChange = () => {};
</script>

<details class="panel">
  <summary>Trigger</summary>
  <div class="panel-content">
    <div class="form-row">
      <label>
        <span class="toggle-label">
          <input type="checkbox" bind:checked={enabled} on:change={onChange}>
          Enabled
        </span>
      </label>
    </div>
    <div class="form-row">
      <label>Source</label>
      <select bind:value={source} on:change={onChange} disabled={!enabled}>
        <option value="A">Channel A</option>
        <option value="B">Channel B</option>
      </select>
    </div>
    <div class="form-row">
      <label>Level</label>
      <input type="number" bind:value={threshold} on:change={onChange}
             disabled={!enabled} step="100" placeholder="mV">
    </div>
    <div class="form-row">
      <label>Edge</label>
      <select bind:value={direction} on:change={onChange} disabled={!enabled}>
        <option value="rising">Rising</option>
        <option value="falling">Falling</option>
      </select>
    </div>
    <div class="form-row">
      <label>Auto ms</label>
      <input type="number" bind:value={autoMs} on:change={onChange}
             disabled={!enabled} min="0" step="100">
    </div>
  </div>
</details>
