<script>
  import { fmtMv, fmtMeas } from '../utils/format.js';

  export let measA = null;
  export let measB = null;
  export let measM = null;
  export let measKeys = new Set();
  export let catalog = [];
  export let statsADisplay = null;
  export let statsBDisplay = null;
</script>

<div class="measurements-bar">
  {#if measA}
    <div class="meas-group">
      <span class="meas-ch-a">CH A</span>
      {#each catalog as m}
        {#if measKeys.has(m.key)}
          <span class="meas-label">{m.label}:</span>
          <span class="meas-value">{fmtMeas(m.fmt, measA[m.key])}</span>
        {/if}
      {/each}
      {#if statsADisplay && statsADisplay.vpp && measKeys.has('vpp')}
        <span class="meas-stats">(Vpp μ={fmtMv(statsADisplay.vpp.avg)} σ=[{fmtMv(statsADisplay.vpp.min)}..{fmtMv(statsADisplay.vpp.max)}] n={statsADisplay.vpp.n})</span>
      {/if}
    </div>
  {/if}
  {#if measB}
    <div class="meas-group">
      <span class="meas-ch-b">CH B</span>
      {#each catalog as m}
        {#if measKeys.has(m.key)}
          <span class="meas-label">{m.label}:</span>
          <span class="meas-value">{fmtMeas(m.fmt, measB[m.key])}</span>
        {/if}
      {/each}
      {#if statsBDisplay && statsBDisplay.vpp && measKeys.has('vpp')}
        <span class="meas-stats">(Vpp μ={fmtMv(statsBDisplay.vpp.avg)} σ=[{fmtMv(statsBDisplay.vpp.min)}..{fmtMv(statsBDisplay.vpp.max)}] n={statsBDisplay.vpp.n})</span>
      {/if}
    </div>
  {/if}
  {#if measM}
    <div class="meas-group">
      <span class="meas-ch-m">MATH</span>
      {#each catalog as m}
        {#if measKeys.has(m.key)}
          <span class="meas-label">{m.label}:</span>
          <span class="meas-value">{fmtMeas(m.fmt, measM[m.key])}</span>
        {/if}
      {/each}
    </div>
  {/if}
  {#if !measA && !measB}
    <span class="meas-label">No data</span>
  {/if}
</div>
