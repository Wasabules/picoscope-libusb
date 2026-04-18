<script>
  /**
   * Bottom log panel — terminal-style view of the currently active decoder's
   * output. To keep WebKit happy under streaming load, this renders a
   * SINGLE <pre> text node that we rebuild on each decode. No reactive each
   * block, no per-row DOM — just one big string swap.
   */
  export let decoderState = { decoder: null, events: [], error: '' };
  export let collapsed = false;

  const DISPLAY_LIMIT = 200;

  let mode = 'table';        // 'table' | 'stream'
  let hexView = true;
  let autoScroll = true;
  let filterKind = 'all';    // 'all' | 'byte' | 'error'
  let logEl;

  $: decoder = decoderState.decoder;
  $: events  = decoderState.events || [];
  $: error   = decoderState.error  || '';
  $: counts  = tallyCounts(events);

  $: text = renderText(events, mode, hexView, filterKind);

  function tallyCounts(evs) {
    let b = 0, e = 0, o = 0;
    for (const x of evs) {
      if (x.kind === 'byte') b++;
      else if (x.kind === 'error') e++;
      else o++;
    }
    return { byte: b, error: e, other: o };
  }

  function fmtTime(ns) {
    if (ns == null || !Number.isFinite(ns)) return '-';
    const abs = Math.abs(ns);
    if (abs < 1e3) return ns.toFixed(0) + ' ns';
    if (abs < 1e6) return (ns / 1e3).toFixed(2) + ' µs';
    if (abs < 1e9) return (ns / 1e6).toFixed(2) + ' ms';
    return (ns / 1e9).toFixed(2) + ' s';
  }

  function renderText(evs, mode, asHex, kindFilter) {
    if (!evs || evs.length === 0) return '';

    if (mode === 'stream') {
      const parts = [];
      const n = evs.length;
      const start = Math.max(0, n - DISPLAY_LIMIT * 4);
      for (let i = start; i < n; i++) {
        const e = evs[i];
        if (e.kind !== 'byte' || typeof e.value !== 'number') continue;
        if (asHex) {
          parts.push(e.value.toString(16).padStart(2, '0').toUpperCase());
        } else {
          parts.push((e.value >= 0x20 && e.value < 0x7f)
            ? String.fromCharCode(e.value) : '.');
        }
      }
      return asHex ? parts.join(' ') : parts.join('');
    }

    // Table mode — format each visible event on its own line.
    const filtered = [];
    for (let i = evs.length - 1; i >= 0 && filtered.length < DISPLAY_LIMIT; i--) {
      const e = evs[i];
      if (kindFilter !== 'all' && e.kind !== kindFilter) continue;
      filtered.push(e);
    }
    filtered.reverse();

    const lines = ['TIME        KIND    HEX   ASCII  TEXT'];
    for (const e of filtered) {
      const t = fmtTime(e.t_ns).padStart(11);
      const k = (e.kind || '').padEnd(6);
      const h = (typeof e.value === 'number')
        ? '0x' + e.value.toString(16).padStart(2, '0').toUpperCase()
        : '    ';
      const a = (typeof e.value === 'number'
                 && e.value >= 0x20 && e.value < 0x7f)
        ? String.fromCharCode(e.value) : ' ';
      const body = e.text || e.annotation || '';
      lines.push(`${t}  ${k}  ${h.padEnd(6)}  ${a}      ${body}`);
    }
    return lines.join('\n');
  }

  function copyAll() {
    navigator.clipboard?.writeText(text).catch(() => {});
  }

  // Auto-scroll — one-shot rAF per text change, so layout-thrash stays bounded
  // even if `text` bursts.
  let scrollPending = false;
  $: if (autoScroll && logEl && text && !scrollPending) {
    scrollPending = true;
    requestAnimationFrame(() => {
      scrollPending = false;
      if (logEl) logEl.scrollTop = logEl.scrollHeight;
    });
  }
</script>

<div class="decoder-log" class:collapsed>
  <div class="log-header">
    <button class="hdr-collapse" on:click={() => collapsed = !collapsed}
            title={collapsed ? 'Expand' : 'Collapse'}>
      {collapsed ? '▸' : '▾'}
    </button>
    <span class="hdr-title">
      Decoder Log
      {#if decoder}
        <span class="hdr-protocol">{decoder.name}</span>
      {:else}
        <span class="hdr-none">(no protocol selected)</span>
      {/if}
    </span>

    <span class="hdr-counts">
      <span class="cnt cnt-byte">{counts.byte} bytes</span>
      {#if counts.error > 0}
        <span class="cnt cnt-error">{counts.error} errors</span>
      {/if}
      {#if counts.other > 0}
        <span class="cnt cnt-other">{counts.other} others</span>
      {/if}
    </span>

    {#if !collapsed}
      <div class="hdr-controls">
        <div class="seg">
          <button class:active={mode === 'table'}
                  on:click={() => mode = 'table'}>Table</button>
          <button class:active={mode === 'stream'}
                  on:click={() => mode = 'stream'}>Stream</button>
        </div>

        {#if mode === 'stream'}
          <div class="seg">
            <button class:active={hexView}
                    on:click={() => hexView = true}>Hex</button>
            <button class:active={!hexView}
                    on:click={() => hexView = false}>ASCII</button>
          </div>
        {:else}
          <select bind:value={filterKind} title="Filter events">
            <option value="all">All</option>
            <option value="byte">Bytes</option>
            <option value="error">Errors</option>
          </select>
          <label class="chk">
            <input type="checkbox" bind:checked={autoScroll}>
            Auto-scroll
          </label>
        {/if}

        <button class="ctl" on:click={copyAll} title="Copy to clipboard">Copy</button>
      </div>
    {/if}
  </div>

  {#if !collapsed}
    <div class="log-body">
      {#if error}
        <div class="err">Decode error: {error}</div>
      {:else if !decoder}
        <div class="empty">Select a protocol in the sidebar to start decoding.</div>
      {:else if !text}
        <div class="empty">No events. Waiting for signal…</div>
      {:else}
        <pre class="text" bind:this={logEl}>{text}</pre>
      {/if}
    </div>
  {/if}
</div>

<style>
  .decoder-log {
    display: flex;
    flex-direction: column;
    background: var(--bg-secondary, #1a2233);
    border-top: 1px solid var(--border-color, #2a3a4a);
    font-family: var(--font-mono, monospace);
    font-size: 11px;
    color: var(--text-primary, #e6edf3);
    height: 220px;
    flex-shrink: 0;
  }
  .decoder-log.collapsed { height: auto; }

  .log-header {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 4px 10px;
    background: var(--bg-secondary, #1a2233);
    border-bottom: 1px solid var(--border-color, #2a3a4a);
    min-height: 28px;
    flex-wrap: wrap;
  }
  .decoder-log.collapsed .log-header { border-bottom: none; }

  .hdr-collapse {
    background: none; border: none; color: var(--text-secondary, #aab);
    cursor: pointer; font-size: 12px; padding: 0 4px;
  }
  .hdr-title { font-weight: 600; color: var(--text-primary, #e6edf3); }
  .hdr-protocol {
    color: var(--accent-cyan, #66ddff);
    margin-left: 6px; font-weight: normal;
  }
  .hdr-none { color: var(--text-muted, #667788); font-weight: normal; }

  .hdr-counts { display: flex; gap: 10px; margin-left: 6px; }
  .cnt { color: var(--text-secondary, #8899aa); }
  .cnt-byte  { color: var(--accent-green, #66cc88); }
  .cnt-error { color: var(--accent-red, #ff6666); }
  .cnt-other { color: var(--accent-yellow, #ffcc66); }

  .hdr-controls {
    display: flex; align-items: center; gap: 6px; margin-left: auto;
    flex-wrap: wrap;
  }
  .hdr-controls .seg { display: inline-flex; }
  .hdr-controls .seg button {
    background: var(--bg-primary, #0f1420);
    border: 1px solid var(--border-color, #2a3a4a);
    color: var(--text-secondary, #8899aa);
    padding: 2px 8px; font-size: 11px; cursor: pointer;
    font-family: inherit;
  }
  .hdr-controls .seg button:first-child  { border-radius: 3px 0 0 3px; }
  .hdr-controls .seg button:last-child   { border-radius: 0 3px 3px 0; border-left: none; }
  .hdr-controls .seg button.active {
    background: var(--accent-cyan, #66ddff); color: #000; border-color: var(--accent-cyan, #66ddff);
  }
  .hdr-controls select {
    background: var(--bg-primary, #0f1420);
    border: 1px solid var(--border-color, #2a3a4a);
    color: var(--text-primary, #e6edf3);
    padding: 2px 6px; border-radius: 3px; font-family: inherit;
    font-size: 11px;
  }
  .hdr-controls .chk {
    color: var(--text-secondary, #8899aa);
    display: inline-flex; align-items: center; gap: 3px;
  }
  .hdr-controls .ctl {
    background: var(--bg-primary, #0f1420);
    border: 1px solid var(--border-color, #2a3a4a);
    color: var(--text-secondary, #8899aa);
    padding: 2px 8px; border-radius: 3px; cursor: pointer;
    font-family: inherit; font-size: 11px;
  }
  .hdr-controls .ctl:hover { color: var(--accent-cyan, #66ddff); }

  .log-body {
    flex: 1; min-height: 0; overflow: hidden; background: #0a0e17;
  }

  .empty, .err { padding: 12px; color: var(--text-muted, #667788); }
  .err { color: var(--accent-red, #ff6666); }

  .text {
    margin: 0;
    padding: 8px 12px;
    font-size: 11px;
    line-height: 1.4;
    color: var(--text-primary, #e6edf3);
    height: 100%;
    overflow: auto;
    white-space: pre;
    word-break: normal;
  }
</style>
