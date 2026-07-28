<script>
  let {
    mathOp        = $bindable('none'),
    cursorsOn     = $bindable(false),
    yCursorsOn    = $bindable(false),
    xyMode        = $bindable(false),
    persistenceOn = $bindable(false),
    avgN          = $bindable(1),
    fftOn         = $bindable(false),
    measMenuOpen  = $bindable(false),
    measKeys      = $bindable(new Set()),
    statsEnabled  = false,
    catalog       = [],
    measDefault   = [],

    canExportCsv = false,
    connected    = false,
    isStreaming  = false,

    onToggleStats   = () => {},
    onToggleMeasKey = () => {},
    onExportCSV     = () => {},
    onExportPNG     = () => {},
    onAuto          = () => {},
  } = $props();
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
          onclick={() => cursorsOn = !cursorsOn}
          title="Vertical cursors (Δt / 1÷Δt)">Δt</button>

  <button class="ctl-btn" class:active={yCursorsOn}
          onclick={() => yCursorsOn = !yCursorsOn}
          title="Horizontal cursors (ΔV)">ΔV</button>

  <button class="ctl-btn" class:active={xyMode}
          onclick={() => xyMode = !xyMode}
          title="X-Y Lissajous mode (A vs B)">XY</button>

  <button class="ctl-btn" class:active={persistenceOn}
          onclick={() => persistenceOn = !persistenceOn}
          title="Phosphor-like overlay">Persist</button>

  <span class="ctl-label">Avg</span>
  <select class="select-inline" bind:value={avgN}>
    <option value={1}>1</option>
    <option value={4}>4</option>
    <option value={16}>16</option>
    <option value={64}>64</option>
  </select>

  <button class="ctl-btn" class:active={fftOn}
          onclick={() => fftOn = !fftOn}
          title="Show FFT spectrum of CH A">FFT</button>

  <button class="ctl-btn" class:active={statsEnabled}
          onclick={onToggleStats}
          title="Accumulate min/max/avg of each measurement over last 50 captures">Stats</button>

  <div class="meas-picker-wrap">
    <button class="ctl-btn" class:active={measMenuOpen}
            onclick={() => measMenuOpen = !measMenuOpen}
            title="Choose which measurements appear in the bottom bar">Meas ▾</button>
    {#if measMenuOpen}
      <div class="meas-picker" onclick={(e) => e.stopPropagation()}>
        <div class="meas-picker-title">Amplitude</div>
        {#each catalog.filter(m => m.group === 'amp') as m}
          <label class="meas-picker-row">
            <input type="checkbox" checked={measKeys.has(m.key)}
                   onchange={() => onToggleMeasKey(m.key)} />
            <span>{m.label}</span>
          </label>
        {/each}
        <div class="meas-picker-title">Time</div>
        {#each catalog.filter(m => m.group === 'time') as m}
          <label class="meas-picker-row">
            <input type="checkbox" checked={measKeys.has(m.key)}
                   onchange={() => onToggleMeasKey(m.key)} />
            <span>{m.label}</span>
          </label>
        {/each}
        <div class="meas-picker-actions">
          <button class="ctl-btn" onclick={() => measKeys = new Set(catalog.map(m => m.key))}>All</button>
          <button class="ctl-btn" onclick={() => measKeys = new Set(measDefault)}>Default</button>
          <button class="ctl-btn" onclick={() => measKeys = new Set()}>None</button>
        </div>
      </div>
    {/if}
  </div>

  <button class="ctl-btn" onclick={onExportCSV}
          disabled={!canExportCsv}
          title="Export visible slice as CSV">CSV</button>

  <button class="ctl-btn" onclick={onExportPNG}
          title="Save scope screenshot">PNG</button>

  <button class="ctl-btn" onclick={onAuto}
          disabled={!connected || isStreaming}
          title="Analyse signal and auto-configure range + timebase + trigger">Auto</button>
</div>
