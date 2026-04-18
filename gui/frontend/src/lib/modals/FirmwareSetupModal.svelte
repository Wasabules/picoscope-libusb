<script>
  import { createEventDispatcher, onMount, onDestroy } from 'svelte';
  import {
    FirmwareStatus, ExtractFirmwareLive, ExtractFirmwareFromPcap, PickPcapFile
  } from '../../../wailsjs/go/main/App.js';
  import { EventsOn } from '../../../wailsjs/runtime/runtime.js';

  export let open = false;

  const dispatch = createEventDispatcher();

  let status = null;    // latest FirmwareStatus result
  let busy = false;
  let error = '';
  let log = [];
  let unsub = null;

  async function refresh() {
    try {
      status = await FirmwareStatus();
    } catch (e) {
      error = String(e);
    }
  }

  function pushLog(line) {
    log = [...log, line].slice(-200);
  }

  onMount(() => {
    unsub = EventsOn('fwExtractLog', pushLog);
    if (open) refresh();
  });
  onDestroy(() => { if (unsub) unsub(); });
  $: if (open) refresh();

  async function runLive() {
    busy = true; error = ''; log = [];
    try {
      await ExtractFirmwareLive();
      await refresh();
      if (status && status.installed) {
        pushLog('[ui] firmware installed — you can close this dialog and connect.');
      }
    } catch (e) {
      error = String(e).replace(/^Error:\s*/, '');
    }
    busy = false;
  }

  async function runPcap() {
    busy = true; error = ''; log = [];
    try {
      const path = await PickPcapFile();
      if (!path) { busy = false; return; }
      pushLog('[ui] selected: ' + path);
      await ExtractFirmwareFromPcap(path);
      await refresh();
    } catch (e) {
      error = String(e).replace(/^Error:\s*/, '');
    }
    busy = false;
  }

  function close() {
    dispatch('close');
  }

  function fmtBytes(n) {
    if (!n) return '—';
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KiB';
    return (n / 1024 / 1024).toFixed(2) + ' MiB';
  }
</script>

{#if open}
<div class="modal-backdrop" on:click|self={close}>
  <div class="modal">
    <div class="modal-header">
      <h2>Firmware setup</h2>
      <button class="icon-btn" on:click={close} title="Close">✕</button>
    </div>

    <div class="modal-body">
      <p class="hint">
        The PicoScope 2204A needs four firmware blobs uploaded at every boot.
        They're copyrighted by Pico Technology and can't be redistributed, but
        you already own a licensed copy — ship with your scope and with the
        PicoSDK. This dialog extracts them from your machine, once.
        Details in <code>docs/firmware-extraction.md</code>.
      </p>

      {#if status}
      <div class="status-grid">
        <div class="s-row">
          <span class="s-label">Target directory</span>
          <code class="s-val">{status.dir || '(none)'}</code>
        </div>
        <div class="s-row">
          <span class="s-label">Installed</span>
          <span class="s-val">
            {#if status.installed}<span class="ok">✓ all four files present</span>
            {:else}<span class="warn">✗ missing files</span>{/if}
          </span>
        </div>
        <div class="s-row">
          <span class="s-label">Files</span>
          <table class="files">
            {#each ['fx2.bin','fpga.bin','waveform.bin','stream_lut.bin'] as f}
              <tr>
                <td>{f}</td>
                <td>{status.files[f] ? fmtBytes(status.files[f]) : '—'}</td>
                <td>{status.files[f] ? '✓' : '✗'}</td>
              </tr>
            {/each}
          </table>
        </div>
      </div>

      <div class="methods">
        <div class="method" class:disabled={!status.sdkAvailable}>
          <h3>Live extraction</h3>
          <p>
            Opens the scope once via the official SDK under
            <code>LD_PRELOAD=libps_intercept.so</code> and captures the
            firmware as it goes over USB.
          </p>
          <ul class="req">
            <li class:req-ok={status.sdkLibPath}>
              SDK lib: {status.sdkLibPath || 'not found — install the Pico suite'}
            </li>
            <li class:req-ok={status.interceptorPath}>
              Interceptor: {status.interceptorPath || 'missing — run `make` in tools/firmware-extractor/'}
            </li>
          </ul>
          <p class="note">
            The scope must be freshly plugged in. If you've connected it in
            this app already, unplug and replug before running.
          </p>
          <button class="btn btn-primary" on:click={runLive}
                  disabled={!status.sdkAvailable || busy}>
            {busy ? 'Extracting…' : 'Extract via SDK'}
          </button>
        </div>

        <div class="method" class:disabled={!status.pcapToolPath || !status.tsharkAvailable}>
          <h3>Offline from pcap</h3>
          <p>
            If you have a <code>usbmon</code> capture from someone who does
            have the SDK installed, extract the firmware from that file. No
            scope needed.
          </p>
          <ul class="req">
            <li class:req-ok={status.pcapToolPath}>
              extract-from-pcap.py: {status.pcapToolPath || 'not found'}
            </li>
            <li class:req-ok={status.tsharkAvailable}>
              tshark: {status.tsharkAvailable ? 'available' : 'missing — apt install tshark'}
            </li>
          </ul>
          <button class="btn" on:click={runPcap}
                  disabled={!status.pcapToolPath || !status.tsharkAvailable || busy}>
            {busy ? 'Extracting…' : 'Pick a pcap…'}
          </button>
        </div>
      </div>

      {#if error}
      <div class="error">{error}</div>
      {/if}

      {#if log.length}
      <div class="log-wrap">
        <div class="log-hdr">Log</div>
        <pre class="log">{log.join('\n')}</pre>
      </div>
      {/if}
      {:else}
      <p class="hint">Loading firmware status…</p>
      {/if}
    </div>

    <div class="modal-footer">
      <button class="btn" on:click={refresh} disabled={busy}>Refresh</button>
      <button class="btn btn-primary" on:click={close}
              disabled={!status || !status.installed}>
        {status && status.installed ? 'Done' : 'Close'}
      </button>
    </div>
  </div>
</div>
{/if}

<style>
  .modal-backdrop {
    position: fixed; inset: 0; background: rgba(0,0,0,0.7);
    display: flex; align-items: center; justify-content: center;
    z-index: 1000;
  }
  .modal {
    background: #0a0e17; color: #e6edf3;
    border: 1px solid #2a3a4a; border-radius: 6px;
    width: min(820px, 95vw); max-height: 90vh;
    display: flex; flex-direction: column;
    box-shadow: 0 10px 40px rgba(0,0,0,0.8);
  }
  .modal-header, .modal-footer {
    display: flex; align-items: center; justify-content: space-between;
    padding: 12px 18px; border-bottom: 1px solid #1e2d3d;
  }
  .modal-footer {
    border-top: 1px solid #1e2d3d; border-bottom: 0;
    justify-content: flex-end; gap: 8px;
  }
  .modal-header h2 { margin: 0; font-size: 16px; }
  .icon-btn {
    background: none; border: 0; color: #8899aa; font-size: 18px;
    cursor: pointer; padding: 4px 8px;
  }
  .icon-btn:hover { color: #fff; }
  .modal-body { padding: 14px 18px; overflow-y: auto; flex: 1; }

  .hint { color: #8899aa; font-size: 12px; margin: 0 0 10px; line-height: 1.5; }
  .hint code { background: #1c2333; padding: 1px 5px; border-radius: 3px; }

  .status-grid {
    background: #111827; padding: 10px 12px; border-radius: 4px;
    margin-bottom: 14px; font-size: 12px;
  }
  .s-row {
    display: flex; gap: 10px; padding: 4px 0; align-items: flex-start;
  }
  .s-label { color: #8899aa; min-width: 140px; }
  .s-val { color: #e6edf3; }
  code.s-val { background: #1c2333; padding: 1px 6px; border-radius: 3px; }
  .ok { color: #00ff88; }
  .warn { color: #ffaa44; }

  table.files { border-collapse: collapse; }
  table.files td {
    padding: 2px 10px 2px 0; font-size: 12px;
  }
  table.files td:first-child { color: #e6edf3; font-family: monospace; }
  table.files td:nth-child(2) { color: #8899aa; }
  table.files td:last-child { color: #00ff88; }

  .methods {
    display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 14px;
  }
  .method {
    background: #111827; border: 1px solid #1e2d3d; border-radius: 4px;
    padding: 12px 14px;
  }
  .method.disabled { opacity: 0.55; }
  .method h3 { margin: 0 0 8px; font-size: 13px; color: #66ddff; }
  .method p { font-size: 12px; color: #c8d4e0; margin: 0 0 8px; line-height: 1.5; }
  .method code { background: #1c2333; padding: 1px 5px; border-radius: 3px; font-size: 11px; }
  .method ul.req { font-size: 11px; color: #ffaa44; margin: 0 0 8px; padding-left: 18px; }
  .method ul.req li { margin: 2px 0; }
  .method ul.req li.req-ok { color: #00ff88; }
  .method .note { color: #ffaa44; font-size: 11px; font-style: italic; }

  .btn {
    background: #1c2333; border: 1px solid #2a3a4a; color: #e6edf3;
    padding: 5px 12px; border-radius: 4px; cursor: pointer; font-size: 12px;
  }
  .btn:disabled { opacity: 0.5; cursor: not-allowed; }
  .btn:hover:not(:disabled) { background: #2a3a4a; }
  .btn-primary { background: #2a4055; border-color: #3a5a75; }
  .btn-primary:hover:not(:disabled) { background: #3a5a75; }

  .error {
    background: #2a1c1c; border-left: 3px solid #ff6666;
    padding: 8px 12px; margin-bottom: 12px; font-size: 12px; color: #ffaabb;
    white-space: pre-wrap;
  }

  .log-wrap { margin-top: 6px; }
  .log-hdr { color: #8899aa; font-size: 11px; margin-bottom: 4px; }
  pre.log {
    background: #05070b; border: 1px solid #1e2d3d; border-radius: 3px;
    padding: 8px 10px; font-size: 11px; color: #a8c0d8;
    max-height: 220px; overflow-y: auto; margin: 0;
    white-space: pre-wrap; word-break: break-word;
  }
</style>
