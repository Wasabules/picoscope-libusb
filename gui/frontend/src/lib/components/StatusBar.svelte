<script>
  import { fmtRate, fmtCount } from '../utils/format.js';

  let {
    connected = false,
    serial = '',
    calDate = '',
    isStreaming = false,
    streamStats = null,
    waveformData = null,
    timebases = [],
    overflow = null,
  } = $props();
</script>

<div class="status-bar">
  <span>
    <span class="status-dot" class:connected class:disconnected={!connected}></span>
    {#if connected}Connected{:else}Disconnected{/if}
  </span>

  {#if serial}<span>Serial: {serial}</span>{/if}
  {#if calDate}<span>Cal: {calDate}</span>{/if}

  {#if isStreaming}
    <span>
      <span class="status-dot streaming"></span>
      Streaming
    </span>
    {#if streamStats}
      <span>{fmtRate(streamStats.samplesPerSec)}</span>
      <span>{fmtCount(streamStats.totalSamples)} samples</span>
      <span>{streamStats.lastBlockMs.toFixed(1)} ms/block</span>
    {/if}
  {/if}

  {#if overflow && (overflow.overA || overflow.overB)}
    <span class="overflow" title="Samples pinned to an ADC rail — the signal exceeds the selected range">
      ⚠ OVER
      {#if overflow.overA}A:{overflow.clippedA}{/if}
      {#if overflow.overB}B:{overflow.clippedB}{/if}
      / {overflow.total}
    </span>
  {/if}

  {#if waveformData && !isStreaming}
    <span>{waveformData.numSamples} samples</span>
    <span>{timebases[waveformData.timebase] ? timebases[waveformData.timebase].label : ''}/sample</span>
  {/if}
</div>
