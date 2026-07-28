<script>
  import { fmtTime } from '../utils/format.js';

  let {
    tdButton = $bindable(null),

    presets = [],
    timePerDivNs = 0,
    tdLabel = 'Auto',
    tdOpen = false,

    offsetActive = false,
    dualRangeActive = false,
    windowStartPct = 0,
    windowEndPct = 100,
    spanNs = 0,

    isStreaming = false,
    streamPaused = false,

    onTdToggle = () => {},
    onTdPick = () => {},
    onShiftDiv = () => {},
    onTogglePause = () => {},
    onFit = () => {},
    onRangeMinInput = () => {},
    onRangeMaxInput = () => {},
  } = $props();
</script>

<div class="display-controls">
  <span class="ctl-label">Time/div</span>

  <div class="ctl-dropdown" bind:this={tdButton}>
    <button type="button" class="ctl-dropdown-trigger"
            onclick={onTdToggle}
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
              onclick={(ev) => onTdPick(p.ns, ev)}>
            {p.label}
          </li>
        {/each}
      </ul>
    {/if}
  </div>

  <button class="ctl-btn" onclick={() => onShiftDiv(-1)}
          disabled={!offsetActive} title="Previous division">◁</button>
  <button class="ctl-btn" onclick={() => onShiftDiv(1)}
          disabled={!offsetActive} title="Next division">▷</button>

  {#if isStreaming}
    <button class="ctl-btn" class:active={streamPaused}
            onclick={onTogglePause}
            title={streamPaused ? 'Resume live roll' : 'Pause streaming view'}>
      {streamPaused ? '▶ Resume' : '⏸ Pause'}
    </button>
  {/if}

  <button class="ctl-btn" onclick={onFit} title="Fit entire buffer">Fit</button>

  <div class="dual-range" class:disabled={!dualRangeActive}
       title="Drag either handle to define the visible window (start / end)">
    <div class="dual-range-track"></div>
    <div class="dual-range-fill"
         style="left:{windowStartPct}%;right:{100 - windowEndPct}%"></div>
    <input type="range" class="dual-range-input dual-range-min"
           min="0" max="100" step="0.1"
           value={windowStartPct}
           oninput={onRangeMinInput}
           disabled={!dualRangeActive}
           aria-label="Window start">
    <input type="range" class="dual-range-input dual-range-max"
           min="0" max="100" step="0.1"
           value={windowEndPct}
           oninput={onRangeMaxInput}
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
