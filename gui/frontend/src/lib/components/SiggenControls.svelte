<script>
  export let waveTypes = [];
  export let wave = 'sine';
  export let freq = 1000;
  export let ampMv = 500;
  export let offsetMv = 0;
  export let duty = 50;
  export let sweep = false;
  export let stopFreq = 10000;
  export let sweepInc = 100;
  export let sweepDwell = 0.1;

  export let connected = false;
  export let enabled = false;

  export let onEnable = () => {};
  export let onDisable = () => {};
</script>

<details class="panel">
  <summary>Signal Generator</summary>
  <div class="panel-content">
    <div class="form-row">
      <label>Wave</label>
      <select bind:value={wave}>
        {#each waveTypes as w}
          <option value={w}>{w}</option>
        {/each}
      </select>
    </div>
    <div class="form-row">
      <label>Freq Hz</label>
      <input type="number" bind:value={freq} min="1" max="100000" step="100">
    </div>
    <div class="form-row">
      <label>Ampl mVpp</label>
      <input type="number" bind:value={ampMv} min="50" max="2000" step="50">
    </div>
    <div class="form-row">
      <label>Offset mV</label>
      <input type="number" bind:value={offsetMv} min="-2000" max="2000" step="50">
    </div>
    {#if wave === 'square'}
      <div class="form-row">
        <label>Duty %</label>
        <input type="number" bind:value={duty} min="1" max="99" step="1">
      </div>
    {/if}
    <div class="form-row">
      <label>
        <span class="toggle-label">
          <input type="checkbox" bind:checked={sweep}>
          Sweep
        </span>
      </label>
    </div>
    {#if sweep}
      <div class="form-row">
        <label>Stop Hz</label>
        <input type="number" bind:value={stopFreq} min="1" max="100000" step="100">
      </div>
      <div class="form-row">
        <label>Step Hz</label>
        <input type="number" bind:value={sweepInc} min="1" max="10000" step="10">
      </div>
      <div class="form-row">
        <label>Dwell s</label>
        <input type="number" bind:value={sweepDwell} min="0.001" max="0.35" step="0.01">
      </div>
    {/if}
    <div class="panel-actions">
      <button class="btn btn-primary" on:click={onEnable} disabled={!connected}>
        {enabled ? 'Update' : 'Enable'}
      </button>
      <button class="btn btn-danger" on:click={onDisable}
              disabled={!connected || !enabled}>
        Disable
      </button>
    </div>
  </div>
</details>
