<script>
  /**
   * Decoder panel (sidebar).
   *
   * Two modes of operation share the same UI:
   *   - Streaming:  the Go backend runs a stateful session fed from the
   *                 streaming goroutine; we listen to the 'decoderEvents'
   *                 Wails event and accumulate history client-side.
   *   - Single-shot: when streaming is off but we have a buffered waveform,
   *                 we call the stateless `Decode()` binding on demand.
   *
   * Events are emitted to the parent via `decode` for the bottom log.
   */
  import { onMount, onDestroy, untrack } from 'svelte';
  import {
    Decode, ListDecoders, StartDecoder, StopDecoder,
  } from '../../../wailsjs/go/main/App.js';
  import { EventsOn } from '../../../wailsjs/runtime/runtime.js';

  let {
    /** @type {Float32Array|number[]|null} */ samplesA = null,
    /** @type {Float32Array|number[]|null} */ samplesB = null,
    rangeMvA = 5000,
    rangeMvB = 5000,
    dtNs = 0,
    isStreaming = false,
    /** When streaming is paused the live session produces nothing, so we
     *  flip to single-shot decoding of the visible slice. */
    paused = false,
    /** Absolute-time (ns) offset of sample 0 in the provided samplesA/B slice.
     *  Used to shift single-shot event times so annotations align with the
     *  zoomed region on the canvas. */
    sliceStartNs = 0,
    /** Optional callback to materialize the *full* streaming ring + dt + ranges
     *  for a one-shot "re-analyze everything" pass. If null, the Re-analyse
     *  button stays disabled. */
    getFullSamples = null,
    /** Emitted whenever the decoder output changes, for the bottom log. */
    ondecode = () => {},
  } = $props();

  /** True when we should feed the streaming backend session. */
  const sessionActive = $derived(isStreaming && !paused);

  /** Max samples sent in a single-shot Decode() — IPC budget. */
  const MAX_DECODE_SAMPLES = 20_000;
  /** Upper bound for the rolling-buffer "Re-analyse" pass. The streaming
   *  ring holds 1 M samples; sending all of them over IPC is fine but we
   *  cap at 512 k to keep latency bounded. */
  const MAX_REANALYZE_SAMPLES = 512_000;
  /** Max events we keep in the rolling history. */
  const HISTORY_MAX = 2_000;

  /** @type {Array<{id:string,name:string,description:string,channels:any[],config:any[]}>} */
  let decoders = $state([]);
  let decoderId = $state('');
  let enabled = $state(false);
  let config = $state({});
  let channelMap = $state({});

  /** Accumulated events (streaming history). */
  let history = $state([]);
  let lastActivityMs = $state(0);
  let unsubEvents;

  onMount(() => {
    ListDecoders().then(list => { decoders = list || []; })
                  .catch(e => console.error('[decoder] ListDecoders', e));

    unsubEvents = EventsOn('decoderEvents', (batch) => {
      if (!batch || batch.length === 0) return;
      lastActivityMs = performance.now();
      history = history.concat(batch);
      if (history.length > HISTORY_MAX) {
        history = history.slice(history.length - HISTORY_MAX);
      }
      dispatchState();
    });
  });

  onDestroy(() => {
    if (unsubEvents) unsubEvents();
    StopDecoder().catch(() => {});
  });

  const decoder = $derived(decoders.find(d => d.id === decoderId) || null);

  // Reset config + map + history only when the selected protocol actually
  // changes — never on every tick. The body is wrapped in `untrack` so the
  // effect depends on `decoder` alone: without it, the `dispatchState()` call
  // would read `history` and make it a dependency, re-running this block on
  // every incoming event batch and silently clobbering any settings the user
  // had just typed in.
  let lastDecoderId = null;
  $effect(() => {
    const currentId = decoder ? decoder.id : null;
    untrack(() => {
      if (currentId === lastDecoderId) return;
      lastDecoderId = currentId;
      if (decoder) {
        const nextCfg = {};
        for (const f of decoder.config) nextCfg[f.key] = f.default;
        config = nextCfg;
        const nextMap = {};
        const avail = ['A', 'B'];
        let idx = 0;
        for (const ch of decoder.channels) {
          nextMap[ch.role] = ch.required && idx < 2 ? avail[idx++] : null;
        }
        channelMap = nextMap;
      } else {
        config = {};
        channelMap = {};
      }
      history = [];
      dispatchState();
    });
  });

  function dispatchState() {
    ondecode({
      decoder, events: history, error: '',
    });
  }

  function clearHistory() {
    history = [];
    dispatchState();
  }

  /** Re-analyse the full rolling buffer in one go. Unlike the live session
   *  (which only sees the trailing tail of what's already been committed),
   *  this scans *everything* the frontend has retained — ideal for hunting
   *  down framed packets whose SOF / EOF happened to fall on opposite sides
   *  of a streaming block boundary. */
  let reanalyzing = $state(false);
  async function reanalyse() {
    if (!enabled || !decoder || !getFullSamples) return;
    if (reanalyzing) return;
    const full = getFullSamples();
    if (!full || !full.samplesA || full.samplesA.length === 0 ||
        !(full.dtNs > 0)) return;
    reanalyzing = true;
    try {
      // Cap for IPC sanity; tail newest so we catch recent traffic.
      const capA = tailFull(full.samplesA);
      const capB = tailFull(full.samplesB);
      const droppedA = Math.max(0, full.samplesA.length - capA.length);
      const startShiftNs = droppedA * full.dtNs;
      const result = await Decode({
        protocol: decoder.id,
        samplesA: capA,
        samplesB: capB,
        dtNs:     full.dtNs,
        rangeMvA: full.rangeMvA,
        rangeMvB: full.rangeMvB,
        config, channelMap,
      });
      const events = (result.events || []).map(e => ({
        ...e,
        t_ns:     (e.t_ns ?? 0) + startShiftNs,
        t_end_ns: e.t_end_ns != null ? e.t_end_ns + startShiftNs : undefined,
      }));
      history = events.slice(-HISTORY_MAX);
      dispatchState();
    } catch (e) {
      console.error('[decoder] reanalyse', e);
    } finally {
      reanalyzing = false;
    }
  }

  function tailFull(arr) {
    if (!arr || arr.length === 0) return [];
    const start = Math.max(0, arr.length - MAX_REANALYZE_SAMPLES);
    const len = arr.length - start;
    const out = new Array(len);
    for (let i = 0; i < len; i++) out[i] = arr[start + i];
    return out;
  }

  /* ---------- streaming session lifecycle ---------- */

  async function syncSession() {
    if (enabled && decoder && sessionActive) {
      try {
        await StartDecoder(decoder.id, config, channelMap);
      } catch (e) {
        console.error('[decoder] StartDecoder', e);
      }
    } else {
      try { await StopDecoder(); } catch {}
    }
  }

  // Start / stop the session when any governing input changes.
  // `$state.snapshot` deep-reads config/channelMap so edits to individual
  // fields register as dependencies — reading the bare reference would not,
  // since mutating a property leaves the proxy identity untouched.
  $effect(() => {
    void [enabled, decoder, sessionActive,
          $state.snapshot(config), $state.snapshot(channelMap)];
    untrack(() => syncSession());
  });

  /* ---------- single-shot decode (when not streaming) ---------- */

  let inflight = $state(false);

  function tailPlain(arr) {
    if (!arr || arr.length === 0) return [];
    const start = Math.max(0, arr.length - MAX_DECODE_SAMPLES);
    const len = arr.length - start;
    const out = new Array(len);
    for (let i = 0; i < len; i++) out[i] = arr[start + i];
    return out;
  }

  async function runSingleShot() {
    if (!enabled || !decoder || sessionActive) return;
    if (!samplesA || samplesA.length === 0 || !(dtNs > 0)) return;
    if (inflight) return;
    inflight = true;
    try {
      const result = await Decode({
        protocol: decoder.id,
        samplesA: tailPlain(samplesA),
        samplesB: tailPlain(samplesB),
        dtNs, rangeMvA, rangeMvB, config, channelMap,
      });
      // Shift event times into absolute-slice coordinates so the canvas'
      // startTimeNs comparison places annotations on the right samples.
      const events = (result.events || []).map(e => ({
        ...e,
        t_ns:     (e.t_ns ?? 0) + sliceStartNs,
        t_end_ns: e.t_end_ns != null ? e.t_end_ns + sliceStartNs : undefined,
      }));
      history = events.slice(-HISTORY_MAX);
      dispatchState();
    } catch (e) {
      console.error('[decoder] single-shot', e);
    } finally {
      inflight = false;
    }
  }

  // Run whenever the single-shot inputs change (incl. paused=true while
  // streaming — the live ring is frozen so we decode the visible window).
  $effect(() => {
    void [enabled, decoder, sessionActive, samplesA, dtNs, sliceStartNs,
          $state.snapshot(config), $state.snapshot(channelMap)];
    untrack(() => { if (!sessionActive) runSingleShot(); });
  });
</script>

<div class="decoder-panel">
  <div class="row">
    <label>Protocol</label>
    <select bind:value={decoderId}>
      <option value="">— none —</option>
      {#each decoders as d}
        <option value={d.id}>{d.name}</option>
      {/each}
    </select>
  </div>

  {#if decoder}
    <p class="desc">{decoder.description}</p>

    <div class="row enable-row">
      <label><input type="checkbox" bind:checked={enabled}>
        Enable decoding</label>
      <div class="enable-actions">
        <button class="clear-btn" onclick={reanalyse}
                disabled={!enabled || !getFullSamples || reanalyzing}
                title="Re-run the decoder on the full rolling buffer">
          {reanalyzing ? '…' : 'Re-analyse'}
        </button>
        <button class="clear-btn" onclick={clearHistory}
                title="Clear history">Clear</button>
      </div>
    </div>
    {#if enabled}
      <div class="status">
        <span>{history.length} events</span>
        {#if isStreaming}
          <span class="dot dot-stream">●</span> streaming session
        {:else}
          <span class="dot dot-shot">●</span> single-shot
        {/if}
      </div>
    {/if}

    <div class="subsection">
      <div class="subtitle">Channel mapping</div>
      {#each decoder.channels as ch}
        <div class="row">
          <label>
            {ch.role}
            {#if !ch.required}<span class="optional">(optional)</span>{/if}
          </label>
          <select bind:value={channelMap[ch.role]}>
            <option value={null}>—</option>
            <option value="A">CH A</option>
            <option value="B">CH B</option>
          </select>
          {#if ch.help}<span class="help">{ch.help}</span>{/if}
        </div>
      {/each}
    </div>

    <div class="subsection">
      <div class="subtitle">Settings</div>
      {#each decoder.config as f}
        <div class="row">
          <label>{f.label}{f.unit ? ' (' + f.unit + ')' : ''}</label>
          {#if f.type === 'number'}
            <input type="number" bind:value={config[f.key]}
                   min={f.min} max={f.max} step={f.step}>
          {:else if f.type === 'select'}
            <select bind:value={config[f.key]}>
              {#each f.options as opt}
                <option value={opt.value}>{opt.label}</option>
              {/each}
            </select>
          {:else if f.type === 'boolean'}
            <input type="checkbox" bind:checked={config[f.key]}>
          {:else if f.type === 'text'}
            <input type="text" bind:value={config[f.key]}
                   placeholder={f.default || ''}>
          {/if}
          {#if f.help}<span class="help">{f.help}</span>{/if}
        </div>
      {/each}
    </div>

    <div class="hint">Decoded messages appear in the log panel below.</div>
  {/if}
</div>

<style>
  .decoder-panel { font-size: 12px; }
  .row {
    display: grid;
    grid-template-columns: 100px 1fr;
    gap: 6px;
    align-items: center;
    margin-bottom: 4px;
  }
  .row label { color: #8899aa; }
  .row input, .row select {
    background: #1c2333;
    border: 1px solid #2a3a4a;
    color: #e6edf3;
    padding: 2px 6px;
    border-radius: 3px;
    font-family: monospace;
  }
  .row .help {
    grid-column: 1 / -1;
    color: #667788;
    font-size: 11px;
    padding-left: 4px;
  }
  .row .optional { color: #667788; font-style: italic; font-size: 11px; }

  .desc { color: #8899aa; font-size: 11px; margin: 4px 0 8px; }

  .subsection { margin-top: 10px; }
  .subtitle {
    font-weight: bold;
    color: #66ddff;
    font-size: 11px;
    text-transform: uppercase;
    margin-bottom: 6px;
    border-bottom: 1px solid #1e2d3d;
    padding-bottom: 2px;
  }

  .hint {
    margin-top: 10px;
    padding: 6px 8px;
    background: #1c2333;
    border-radius: 3px;
    font-size: 11px;
    color: #8899aa;
  }

  .enable-row {
    grid-template-columns: 1fr auto;
    margin: 6px 0 4px;
    padding: 4px 6px;
    background: #1c2333;
    border-radius: 3px;
  }
  .enable-actions { display: flex; gap: 4px; }
  .clear-btn:disabled { opacity: 0.4; cursor: not-allowed; }
  .enable-row label {
    display: flex; align-items: center; gap: 6px;
    color: #e6edf3; cursor: pointer;
  }
  .clear-btn {
    background: #0f1420; border: 1px solid #2a3a4a;
    color: #8899aa; padding: 2px 8px; border-radius: 3px;
    cursor: pointer; font-size: 11px; font-family: inherit;
  }
  .clear-btn:hover { color: #66ddff; }

  .status {
    display: flex; align-items: center; gap: 8px;
    font-size: 10px; color: #8899aa;
    padding: 0 6px 4px;
  }
  .dot { font-size: 13px; line-height: 1; }
  .dot-stream { color: #66cc88; }
  .dot-shot   { color: #ffcc66; }
</style>
