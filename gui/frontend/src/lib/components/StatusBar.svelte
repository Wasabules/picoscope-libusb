<script>
  import { fmtRate, fmtCount } from '../utils/format.js';

  export let connected = false;
  export let serial = '';
  export let calDate = '';
  export let isStreaming = false;
  export let streamStats = null;
  export let waveformData = null;
  export let timebases = [];
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

  {#if waveformData && !isStreaming}
    <span>{waveformData.numSamples} samples</span>
    <span>{timebases[waveformData.timebase] ? timebases[waveformData.timebase].label : ''}/sample</span>
  {/if}
</div>
