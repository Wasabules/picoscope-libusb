<script>
  export let mathOp = 'none';
  export let cursorsOn = false;
  export let yCursorsOn = false;
  export let xyMode = false;
  export let persistenceOn = false;
  export let avgN = 1;
  export let fftOn = false;
  export let statsEnabled = false;
  export let measMenuOpen = false;
  export let measKeys = new Set();
  export let catalog = [];
  export let measDefault = [];

  export let canExportCsv = false;
  export let connected = false;
  export let isStreaming = false;

  export let onToggleStats = () => {};
  export let onToggleMeasKey = () => {};
  export let onExportCSV = () => {};
  export let onExportPNG = () => {};
  export let onAuto = () => {};
</script>

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
          on:click={onToggleStats}
          title="Accumulate min/max/avg of each measurement over last 50 captures">Stats</button>

  <div class="meas-picker-wrap">
    <button class="ctl-btn" class:active={measMenuOpen}
            on:click={() => measMenuOpen = !measMenuOpen}
            title="Choose which measurements appear in the bottom bar">Meas ▾</button>
    {#if measMenuOpen}
      <div class="meas-picker" on:click|stopPropagation>
        <div class="meas-picker-title">Amplitude</div>
        {#each catalog.filter(m => m.group === 'amp') as m}
          <label class="meas-picker-row">
            <input type="checkbox" checked={measKeys.has(m.key)}
                   on:change={() => onToggleMeasKey(m.key)} />
            <span>{m.label}</span>
          </label>
        {/each}
        <div class="meas-picker-title">Time</div>
        {#each catalog.filter(m => m.group === 'time') as m}
          <label class="meas-picker-row">
            <input type="checkbox" checked={measKeys.has(m.key)}
                   on:change={() => onToggleMeasKey(m.key)} />
            <span>{m.label}</span>
          </label>
        {/each}
        <div class="meas-picker-actions">
          <button class="ctl-btn" on:click={() => measKeys = new Set(catalog.map(m => m.key))}>All</button>
          <button class="ctl-btn" on:click={() => measKeys = new Set(measDefault)}>Default</button>
          <button class="ctl-btn" on:click={() => measKeys = new Set()}>None</button>
        </div>
      </div>
    {/if}
  </div>

  <button class="ctl-btn" on:click={onExportCSV}
          disabled={!canExportCsv}
          title="Export visible slice as CSV">CSV</button>

  <button class="ctl-btn" on:click={onExportPNG}
          title="Save scope screenshot">PNG</button>

  <button class="ctl-btn" on:click={onAuto}
          disabled={!connected || isStreaming}
          title="Analyse signal and auto-configure range + timebase + trigger">Auto</button>
</div>
