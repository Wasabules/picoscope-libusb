<script>
  /**
   * Trigger panel. Three modes share the level/source/auto fields:
   *   edge   — the classic rising/falling crossing
   *   window — fire on entering or leaving a voltage band
   *   pulse  — fire on a crossing only when the preceding pulse's width
   *            falls inside a range (runt / glitch hunting)
   */
  let {
    enabled   = $bindable(false),
    source    = $bindable('A'),
    threshold = $bindable(0),
    direction = $bindable('rising'),
    autoMs    = $bindable(0),

    mode        = $bindable('edge'),
    hysteresis  = $bindable(0),
    windowLower = $bindable(-100),
    windowUpper = $bindable(100),
    pulseLowerNs = $bindable(0),
    pulseUpperNs = $bindable(0),

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
      <label>Mode</label>
      <select bind:value={mode} onchange={onChange} disabled={!enabled}>
        <option value="edge">Edge</option>
        <option value="window">Window</option>
        <option value="pulse">Pulse width</option>
      </select>
    </div>

    <div class="form-row">
      <label>Source</label>
      <select bind:value={source} onchange={onChange} disabled={!enabled}>
        <option value="A">Channel A</option>
        <option value="B">Channel B</option>
      </select>
    </div>

    {#if mode === 'window'}
      <div class="form-row">
        <label>Lower mV</label>
        <input type="number" bind:value={windowLower} onchange={onChange}
               disabled={!enabled} step="50">
      </div>
      <div class="form-row">
        <label>Upper mV</label>
        <input type="number" bind:value={windowUpper} onchange={onChange}
               disabled={!enabled} step="50">
      </div>
      <div class="form-row">
        <label>On</label>
        <select bind:value={direction} onchange={onChange} disabled={!enabled}>
          <option value="rising">Entering</option>
          <option value="falling">Leaving</option>
        </select>
      </div>
    {:else}
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
    {/if}

    {#if mode === 'pulse'}
      <div class="form-row">
        <label>Width ≥ ns</label>
        <input type="number" bind:value={pulseLowerNs} onchange={onChange}
               disabled={!enabled} min="0" step="100">
      </div>
      <div class="form-row">
        <label>Width ≤ ns</label>
        <input type="number" bind:value={pulseUpperNs} onchange={onChange}
               disabled={!enabled} min="0" step="100"
               title="0 = no upper bound">
      </div>
    {/if}

    {#if mode === 'edge'}
      <div class="form-row">
        <label>Hysteresis</label>
        <input type="number" bind:value={hysteresis} onchange={onChange}
               disabled={!enabled} min="0" max="64" step="1"
               title="Dead band in ADC codes — stops a noisy edge re-triggering on its own ripple">
      </div>
    {/if}

    <div class="form-row">
      <label>Auto ms</label>
      <input type="number" bind:value={autoMs} onchange={onChange}
             disabled={!enabled} min="0" step="100">
    </div>
  </div>
</details>
