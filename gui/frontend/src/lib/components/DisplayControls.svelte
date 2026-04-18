<script>
  import { fmtTime } from '../utils/format.js';

  export let presets = [];
  export let timePerDivNs = 0;
  export let tdLabel = 'Auto';
  export let tdOpen = false;
  export let tdButton = null;

  export let offsetActive = false;
  export let dualRangeActive = false;
  export let windowStartPct = 0;
  export let windowEndPct = 100;
  export let spanNs = 0;

  export let isStreaming = false;
  export let streamPaused = false;

  export let onTdToggle = () => {};
  export let onTdPick = () => {};
  export let onShiftDiv = () => {};
  export let onTogglePause = () => {};
  export let onFit = () => {};
  export let onRangeMinInput = () => {};
  export let onRangeMaxInput = () => {};
</script>

<div class="display-controls">
  <span class="ctl-label">Time/div</span>

  <div class="ctl-dropdown" bind:this={tdButton}>
    <button type="button" class="ctl-dropdown-trigger"
            on:click={onTdToggle}
            aria-haspopup="listbox" aria-expanded={tdOpen}>
      {tdLabel} <span class="caret">▾</span>
    </button>
    {#if tdOpen}
      <ul class="ctl-dropdown-menu" role="listbox">
        {#each presets as p}
          <li class="ctl-dropdown-item"
              class:selected={p.ns === timePerDivNs}
              role="option"
              aria-selected={p.ns === timePerDivNs}
              on:click={(ev) => onTdPick(p.ns, ev)}>
            {p.label}
          </li>
        {/each}
      </ul>
    {/if}
  </div>

  <button class="ctl-btn" on:click={() => onShiftDiv(-1)}
          disabled={!offsetActive} title="Previous division">◁</button>
  <button class="ctl-btn" on:click={() => onShiftDiv(1)}
          disabled={!offsetActive} title="Next division">▷</button>

  {#if isStreaming}
    <button class="ctl-btn" class:active={streamPaused}
            on:click={onTogglePause}
            title={streamPaused ? 'Resume live roll' : 'Pause streaming view'}>
      {streamPaused ? '▶ Resume' : '⏸ Pause'}
    </button>
  {/if}

  <button class="ctl-btn" on:click={onFit} title="Fit entire buffer">Fit</button>

  <div class="dual-range" class:disabled={!dualRangeActive}
       title="Drag either handle to define the visible window (start / end)">
    <div class="dual-range-track"></div>
    <div class="dual-range-fill"
         style="left:{windowStartPct}%;right:{100 - windowEndPct}%"></div>
    <input type="range" class="dual-range-input dual-range-min"
           min="0" max="100" step="0.1"
           value={windowStartPct}
           on:input={onRangeMinInput}
           disabled={!dualRangeActive}
           aria-label="Window start">
    <input type="range" class="dual-range-input dual-range-max"
           min="0" max="100" step="0.1"
           value={windowEndPct}
           on:input={onRangeMaxInput}
           disabled={!dualRangeActive}
           aria-label="Window end">
  </div>
  <span class="ctl-spanlabel">
    {#if spanNs > 0}
      {fmtTime(spanNs)} span
    {:else}
      —
    {/if}
  </span>
</div>
