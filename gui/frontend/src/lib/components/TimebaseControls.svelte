<script>
  export let timebase = 5;
  export let timebases = [];
  export let samples = 8064;
  export let maxSamples = 8064;
  export let tbLabel = '';
  export let connected = false;
  export let dual = false;

  export let onTimebaseChange = () => {};
  export let onSamplesChange = () => {};
</script>

<details class="panel" open>
  <summary>Timebase</summary>
  <div class="panel-content">
    <div class="form-row">
      <label>TB</label>
      <select bind:value={timebase} on:change={onTimebaseChange}>
        {#each timebases as tb, i}
          <option value={i}>{i} - {tb.label}</option>
        {/each}
        {#if timebases.length === 0}
          {#each Array(11) as _, i}
            <option value={i}>{i}</option>
          {/each}
        {/if}
      </select>
    </div>
    {#if tbLabel}
      <div class="form-row">
        <label>Interval</label>
        <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">{tbLabel}/sample</span>
      </div>
    {/if}
    <div class="form-row">
      <label>Samples</label>
      <input type="number" bind:value={samples}
             on:change={onSamplesChange}
             min="64" max={maxSamples} step="64"
             disabled={!connected}>
    </div>
    <div class="form-row">
      <label>Max</label>
      <span style="color: var(--text-primary); font-family: var(--font-mono); font-size: 12px;">
        {maxSamples} {dual ? '(dual)' : '(single)'}
      </span>
    </div>
  </div>
</details>
