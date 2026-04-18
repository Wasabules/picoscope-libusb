<script>
  import { createEventDispatcher, onMount, onDestroy } from 'svelte';

  /* Pure renderer — parent supplies the already-windowed slice. */
  export let samplesA = null;         // Array|Float32Array of mV samples, or null
  export let samplesB = null;
  export let samplesM = null;         // Math channel samples (optional)
  export let channelAEnabled = true;
  export let channelBEnabled = false;
  export let mathLabel = '';          // '' = no math trace
  export let rangeMvA = 5000;
  export let rangeMvB = 5000;
  /* Optional per-channel vertical offset (mV, subtracted before scaling)
   * and V/div. When vdivA/B > 0, that channel uses its own vertical zoom
   * independent of the range; the Y-axis labels then show the A channel's
   * scale unless xxDivB is toggled. */
  export let offsetMvA = 0;
  export let offsetMvB = 0;
  export let vdivMvA = 0;    // 0 = auto (use range)
  export let vdivMvB = 0;    // 0 = auto (use range)
  export let yCursorsOn = false;
  export let yCursor1Mv = 100;
  export let yCursor2Mv = -100;

  /** @type {Array<{t_ns:number,t_end_ns?:number,kind:string,annotation?:string,text?:string,level?:string}>} */
  export let annotations = null;
  export let startTimeNs = 0;         // time of first sample relative to viewport
  export let spanNs = 0;              // total visible time (ns). 0 = unknown / auto

  export let xyMode = false;          // A vs B Lissajous instead of time traces
  export let cursorsOn = false;
  export let cursor1Pct = 30;
  export let cursor2Pct = 70;
  export let persistenceOn = false;   // composite traces with alpha, don't erase fully

  const dispatch = createEventDispatcher();

  let canvas;
  let ctx;
  let width = 800;
  let height = 500;

  const MARGIN = { top: 10, right: 20, bottom: 35, left: 60 };
  const CH_A_COLOR = '#00ff88';
  const CH_B_COLOR = '#ffd700';
  const CH_M_COLOR = '#ff6ba8';
  const CURSOR_COLOR = '#66ddff';
  const GRID_COLOR = '#1e2d3d';
  const GRID_COLOR_CENTER = '#2a4055';
  const TEXT_COLOR = '#8899aa';
  const BG_COLOR = '#0a0e17';

  const GRID_COLS = 10;
  const GRID_ROWS = 8;

  function fmtTime(ns) {
    const abs = Math.abs(ns);
    if (abs < 1e3)   return ns.toFixed(0) + ' ns';
    if (abs < 1e6)   return (ns / 1e3).toFixed(abs < 1e4 ? 2 : 1) + ' µs';
    if (abs < 1e9)   return (ns / 1e6).toFixed(abs < 1e7 ? 2 : 1) + ' ms';
    return (ns / 1e9).toFixed(abs < 1e10 ? 2 : 1) + ' s';
  }

  function resizeCanvas() {
    if (!canvas) return;
    const rect = canvas.parentElement.getBoundingClientRect();
    width = rect.width;
    height = rect.height;
    canvas.width = width * window.devicePixelRatio;
    canvas.height = height * window.devicePixelRatio;
    canvas.style.width = width + 'px';
    canvas.style.height = height + 'px';
    ctx = canvas.getContext('2d');
    ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
    draw();
  }

  function draw() {
    if (!ctx) return;

    const gx = MARGIN.left;
    const gy = MARGIN.top;
    const gw = width - MARGIN.left - MARGIN.right;
    const gh = height - MARGIN.top - MARGIN.bottom;

    if (persistenceOn && !xyMode) {
      // Fade previous frame slightly instead of clearing.
      ctx.fillStyle = 'rgba(10, 14, 23, 0.15)';
      ctx.fillRect(0, 0, width, height);
    } else {
      ctx.fillStyle = BG_COLOR;
      ctx.fillRect(0, 0, width, height);
    }

    // Grid
    ctx.lineWidth = 1;
    for (let i = 0; i <= GRID_COLS; i++) {
      const x = gx + (gw * i / GRID_COLS);
      ctx.strokeStyle = (i === GRID_COLS / 2) ? GRID_COLOR_CENTER : GRID_COLOR;
      ctx.lineWidth = (i === GRID_COLS / 2) ? 1.5 : 0.5;
      ctx.beginPath();
      ctx.moveTo(x, gy);
      ctx.lineTo(x, gy + gh);
      ctx.stroke();
    }
    for (let i = 0; i <= GRID_ROWS; i++) {
      const y = gy + (gh * i / GRID_ROWS);
      ctx.strokeStyle = (i === GRID_ROWS / 2) ? GRID_COLOR_CENTER : GRID_COLOR;
      ctx.lineWidth = (i === GRID_ROWS / 2) ? 1.5 : 0.5;
      ctx.beginPath();
      ctx.moveTo(gx, y);
      ctx.lineTo(gx + gw, y);
      ctx.stroke();
    }

    ctx.strokeStyle = '#2a3a4a';
    ctx.lineWidth = 1;
    ctx.strokeRect(gx, gy, gw, gh);

    // Y-axis labels (CH A range)
    const activeRange = rangeMvA;
    ctx.fillStyle = TEXT_COLOR;
    ctx.font = '11px monospace';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let i = 0; i <= GRID_ROWS; i++) {
      const y = gy + (gh * i / GRID_ROWS);
      const mv = activeRange * (1 - 2 * i / GRID_ROWS);
      let label;
      if (Math.abs(mv) >= 1000) {
        label = (mv / 1000).toFixed(1) + ' V';
      } else {
        label = mv.toFixed(0) + ' mV';
      }
      ctx.fillText(label, gx - 5, y);
    }

    // X-axis labels derived from spanNs / startTimeNs
    if (spanNs > 0) {
      ctx.textAlign = 'center';
      ctx.textBaseline = 'top';
      for (let i = 0; i <= GRID_COLS; i++) {
        const x = gx + (gw * i / GRID_COLS);
        const ns = startTimeNs + spanNs * i / GRID_COLS;
        ctx.fillText(fmtTime(ns), x, gy + gh + 5);
      }
    }

    // Traces (time or XY mode)
    if (xyMode && samplesA && samplesB && samplesA.length > 0 && samplesB.length > 0) {
      drawXY(ctx, samplesA, samplesB, rangeMvA, rangeMvB, gx, gy, gw, gh);
    } else {
      // Each channel gets its own (scale, offset) so it can be zoomed &
      // positioned independently. Scale is (gh/2) / half-range, where the
      // effective half-range = vdivMv × (GRID_ROWS/2) if vdivMv>0, else
      // rangeMv.
      const halfA = vdivMvA > 0 ? vdivMvA * (GRID_ROWS / 2) : rangeMvA;
      const halfB = vdivMvB > 0 ? vdivMvB * (GRID_ROWS / 2) : rangeMvB;
      if (channelAEnabled && samplesA && samplesA.length > 0) {
        drawTrace(ctx, samplesA, halfA, offsetMvA, CH_A_COLOR, gx, gy, gw, gh);
      }
      if (channelBEnabled && samplesB && samplesB.length > 0) {
        drawTrace(ctx, samplesB, halfB, offsetMvB, CH_B_COLOR, gx, gy, gw, gh);
      }
      // Math channel — scale by the largest range so it fits the screen
      if (samplesM && samplesM.length > 0) {
        const rangeM = Math.max(halfA, halfB) * 2;
        drawTrace(ctx, samplesM, rangeM, 0, CH_M_COLOR, gx, gy, gw, gh);
      }
    }

    // Protocol-decoder annotations (drawn below traces so they don't
     // obscure the signal — a ribbon of labels at the top of the plot).
    if (annotations && annotations.length > 0 && spanNs > 0 && !xyMode) {
      drawAnnotations(ctx, gx, gy, gw, gh);
    }

    // Vertical (time) cursors — Δt
    if (cursorsOn && !xyMode) {
      drawCursors(ctx, gx, gy, gw, gh);
    }
    // Horizontal (voltage) cursors — ΔV. Uses CH A's scale so the mV
    // values correspond to whatever rangeMvA / vdivMvA is active.
    if (yCursorsOn && !xyMode) {
      drawYCursors(ctx, gx, gy, gw, gh);
    }

    ctx.font = 'bold 12px monospace';
    ctx.textAlign = 'left';
    let labelY = gy + 15;
    if (xyMode) {
      ctx.fillStyle = CH_A_COLOR;
      ctx.fillText('XY — X: A, Y: B', gx + 5, labelY);
    } else {
      if (channelAEnabled) {
        const tag = isClipping(samplesA, rangeMvA) ? ' [CLIP!]' : '';
        ctx.fillStyle = tag ? '#ff4444' : CH_A_COLOR;
        ctx.fillText('CH A' + tag, gx + 5, labelY); labelY += 15;
      }
      if (channelBEnabled) {
        const tag = isClipping(samplesB, rangeMvB) ? ' [CLIP!]' : '';
        ctx.fillStyle = tag ? '#ff4444' : CH_B_COLOR;
        ctx.fillText('CH B' + tag, gx + 5, labelY); labelY += 15;
      }
      if (samplesM && mathLabel) {
        ctx.fillStyle = CH_M_COLOR;
        ctx.fillText('MATH (' + mathLabel + ')', gx + 5, labelY);
      }
    }

    // Red outline when ANY channel is clipping — grabs attention.
    const anyClip =
      (channelAEnabled && isClipping(samplesA, rangeMvA)) ||
      (channelBEnabled && isClipping(samplesB, rangeMvB));
    if (anyClip) {
      ctx.strokeStyle = '#ff4444';
      ctx.lineWidth = 2;
      ctx.strokeRect(gx, gy, gw, gh);
    }

    // Rubber-band overlay for box-zoom selection.
    if (zoomStartX !== null && zoomCurX !== null) {
      const x0 = Math.min(zoomStartX, zoomCurX);
      const x1 = Math.max(zoomStartX, zoomCurX);
      ctx.fillStyle = 'rgba(102, 221, 255, 0.18)';
      ctx.fillRect(gx + x0, gy, x1 - x0, gh);
      ctx.strokeStyle = '#66ddff';
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      ctx.strokeRect(gx + x0, gy, x1 - x0, gh);
      ctx.setLineDash([]);
    }
  }

  // A channel is "clipping" when ≥ 3 samples sit within 1 % of either rail.
  // The 1 % margin catches the 8-bit ADC's saturated bytes (0x00 / 0xff).
  function isClipping(data, rangeMv) {
    if (!data || data.length === 0) return false;
    const threshold = rangeMv * 0.99;
    let count = 0;
    for (let i = 0; i < data.length; i++) {
      if (data[i] >= threshold || data[i] <= -threshold) {
        if (++count >= 3) return true;
      }
    }
    return false;
  }

  function drawXY(ctx, dataA, dataB, rangeA, rangeB, gx, gy, gw, gh) {
    const n = Math.min(dataA.length, dataB.length);
    if (n === 0) return;
    ctx.strokeStyle = CH_A_COLOR;
    ctx.lineWidth = 1;
    ctx.beginPath();
    const cx = gx + gw / 2;
    const cy = gy + gh / 2;
    const sx = (gw / 2) / rangeA;
    const sy = (gh / 2) / rangeB;
    for (let i = 0; i < n; i++) {
      const x = Math.max(gx, Math.min(gx + gw, cx + dataA[i] * sx));
      const y = Math.max(gy, Math.min(gy + gh, cy - dataB[i] * sy));
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.stroke();
  }

  function drawCursors(ctx, gx, gy, gw, gh) {
    const x1 = gx + gw * (cursor1Pct / 100);
    const x2 = gx + gw * (cursor2Pct / 100);
    ctx.strokeStyle = CURSOR_COLOR;
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath(); ctx.moveTo(x1, gy); ctx.lineTo(x1, gy + gh); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(x2, gy); ctx.lineTo(x2, gy + gh); ctx.stroke();
    ctx.setLineDash([]);
    // Delta readout
    if (spanNs > 0) {
      const dt = Math.abs(cursor2Pct - cursor1Pct) / 100 * spanNs;
      const freq = dt > 0 ? 1e9 / dt : 0;
      ctx.fillStyle = CURSOR_COLOR;
      ctx.textAlign = 'right';
      ctx.textBaseline = 'top';
      ctx.font = 'bold 11px monospace';
      const txt = 'Δt = ' + fmtTime(dt) + '   1/Δt = ' + (freq > 0 ? freq.toFixed(1) + ' Hz' : '—');
      ctx.fillText(txt, gx + gw - 5, gy + 5);
    }
  }

  // Draw protocol-decoder annotations as a thin ribbon across the top
  // of the plot area. Span-style events (byte, frame) show as a bracket;
  // point events (START, STOP, ACK) show as a vertical tick with label.
  function drawAnnotations(ctx, gx, gy, gw, gh) {
    if (!spanNs || spanNs <= 0 || !Number.isFinite(spanNs)) return;
    const ribbonH = 18;
    const baseY = gy + 2;
    // Hard cap — we just draw the last N annotations visible in the window.
    const MAX_DRAW = 50;
    const src = annotations.length > MAX_DRAW
      ? annotations.slice(annotations.length - MAX_DRAW)
      : annotations;
    ctx.save();
    ctx.font = '10px monospace';
    ctx.textBaseline = 'middle';
    for (const a of src) {
      const t_ns    = +a.t_ns;
      const t_e_ns  = a.t_end_ns != null ? +a.t_end_ns : t_ns;
      if (!Number.isFinite(t_ns) || !Number.isFinite(t_e_ns)) continue;
      const t0 = t_ns - startTimeNs;
      const t1 = t_e_ns - startTimeNs;
      if (t1 < 0 || t0 > spanNs) continue;
      const x0raw = (t0 / spanNs) * gw;
      const x1raw = (t1 / spanNs) * gw;
      if (!Number.isFinite(x0raw) || !Number.isFinite(x1raw)) continue;
      const x0 = gx + Math.max(0, x0raw);
      const x1 = gx + Math.min(gw, x1raw);
      const color = a.level === 'error' ? '#ff5566'
                  : a.level === 'warn'  ? '#ffaa55'
                  :                        '#66ddff';
      ctx.strokeStyle = color;
      ctx.fillStyle   = color;
      if (x1 > x0 + 1) {
        // Span event (byte, frame): draw a rounded bracket.
        ctx.lineWidth = 1;
        ctx.strokeRect(x0, baseY, x1 - x0, ribbonH);
      } else {
        // Point event: vertical tick.
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(x0, baseY);
        ctx.lineTo(x0, baseY + ribbonH);
        ctx.stroke();
      }
      const lbl = a.annotation || a.text || '';
      if (lbl) {
        ctx.textAlign = 'left';
        ctx.fillText(lbl, x0 + 3, baseY + ribbonH / 2);
      }
    }
    ctx.restore();
  }

  function drawYCursors(ctx, gx, gy, gw, gh) {
    const halfA = vdivMvA > 0 ? vdivMvA * (GRID_ROWS / 2) : rangeMvA;
    const centerY = gy + gh / 2;
    const sc = (gh / 2) / halfA;
    const y1 = centerY - (yCursor1Mv + offsetMvA) * sc;
    const y2 = centerY - (yCursor2Mv + offsetMvA) * sc;
    ctx.strokeStyle = '#ffaa55';
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath(); ctx.moveTo(gx, y1); ctx.lineTo(gx + gw, y1); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(gx, y2); ctx.lineTo(gx + gw, y2); ctx.stroke();
    ctx.setLineDash([]);
    const dv = Math.abs(yCursor2Mv - yCursor1Mv);
    ctx.fillStyle = '#ffaa55';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'top';
    ctx.font = 'bold 11px monospace';
    const txt = 'ΔV = ' + (Math.abs(dv) >= 1000 ? (dv/1000).toFixed(3) + ' V' : dv.toFixed(1) + ' mV');
    ctx.fillText(txt, gx + gw - 5, gy + 20);
  }

  function drawTrace(ctx, data, halfRangeMv, offsetMv, color, gx, gy, gw, gh) {
    const n = data.length;
    if (n === 0) return;

    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.beginPath();

    // offsetMv moves the trace UP on screen (so value+offset = visual center).
    const centerY = gy + gh / 2;
    const scaleY = (gh / 2) / halfRangeMv;

    // If we have more samples than pixels, draw min/max per column for a proper envelope.
    if (n > gw * 2) {
      const samplesPerPx = n / gw;
      for (let px = 0; px < gw; px++) {
        const start = Math.floor(px * samplesPerPx);
        const end = Math.min(n, Math.floor((px + 1) * samplesPerPx));
        let mn = data[start], mx = data[start];
        for (let i = start + 1; i < end; i++) {
          const v = data[i];
          if (v < mn) mn = v;
          if (v > mx) mx = v;
        }
        const x = gx + px;
        const yTop = Math.max(gy, Math.min(gy + gh, centerY - (mx + offsetMv) * scaleY));
        const yBot = Math.max(gy, Math.min(gy + gh, centerY - (mn + offsetMv) * scaleY));
        if (px === 0) ctx.moveTo(x, yTop);
        ctx.lineTo(x, yTop);
        ctx.lineTo(x, yBot);
      }
    } else {
      for (let i = 0; i < n; i++) {
        const x = gx + (i / (n - 1 || 1)) * gw;
        const y = centerY - (data[i] + offsetMv) * scaleY;
        const yc = Math.max(gy, Math.min(gy + gh, y));
        if (i === 0) ctx.moveTo(x, yc);
        else ctx.lineTo(x, yc);
      }
    }
    ctx.stroke();
  }

  /* -------- Mouse interactions -------- */

  function plotRect() {
    return {
      x: MARGIN.left,
      y: MARGIN.top,
      w: width - MARGIN.left - MARGIN.right,
      h: height - MARGIN.top - MARGIN.bottom,
    };
  }

  function onWheel(ev) {
    const r = plotRect();
    const rect = canvas.getBoundingClientRect();
    const localX = ev.clientX - rect.left - r.x;
    if (localX < 0 || localX > r.w) return;
    ev.preventDefault();
    dispatch('zoom', {
      deltaY: ev.deltaY,
      fracX: localX / r.w,
    });
  }

  let dragLastX = null;
  let dragCursor = 0; // 0 = pan, 1 = cursor1, 2 = cursor2
  // Box-zoom (shift+drag or right-click+drag): rubber-band a region and
  // emit a 'zoomTo' event with normalized {startFrac, endFrac} so the
  // parent can reframe to exactly that slice. Right-click works too so
  // the feature is reachable without the keyboard.
  let zoomStartX = null;   // pixel X of initial click (plot-local)
  let zoomCurX  = null;
  function onPointerDown(ev) {
    if (ev.button !== 0 && ev.button !== 2) return;
    const r = plotRect();
    const rect = canvas.getBoundingClientRect();
    const localX = ev.clientX - rect.left - r.x;
    if (localX < 0 || localX > r.w) return;
    const localY = ev.clientY - rect.top - r.y;
    // Box-zoom when shift held or right-click. Wins over pan/cursor drag.
    if (ev.shiftKey || ev.button === 2) {
      ev.preventDefault();
      zoomStartX = localX;
      zoomCurX = localX;
      canvas.setPointerCapture(ev.pointerId);
      canvas.style.cursor = 'zoom-in';
      // Parent auto-pauses streaming so the ring doesn't roll under the
      // selection during the drag — otherwise the released rectangle
      // maps to different absolute times than what the user saw.
      dispatch('zoomBegin');
      draw();
      return;
    }
    // Check proximity to any cursor (vertical or horizontal). Y-cursor
    // drag is only possible when clicking roughly on its horizontal line.
    dragCursor = 0;
    if (cursorsOn && !xyMode) {
      const x1 = r.w * (cursor1Pct / 100);
      const x2 = r.w * (cursor2Pct / 100);
      if (Math.abs(localX - x1) <= 8) dragCursor = 1;
      else if (Math.abs(localX - x2) <= 8) dragCursor = 2;
    }
    if (!dragCursor && yCursorsOn && !xyMode) {
      const halfA = vdivMvA > 0 ? vdivMvA * (GRID_ROWS / 2) : rangeMvA;
      const sc = (r.h / 2) / halfA;
      const y1 = r.h / 2 - (yCursor1Mv + offsetMvA) * sc;
      const y2 = r.h / 2 - (yCursor2Mv + offsetMvA) * sc;
      if (Math.abs(localY - y1) <= 8) dragCursor = 3;
      else if (Math.abs(localY - y2) <= 8) dragCursor = 4;
    }
    dragLastX = ev.clientX;
    canvas.setPointerCapture(ev.pointerId);
    canvas.style.cursor = dragCursor ? 'ew-resize' : 'grabbing';
  }

  function onPointerMove(ev) {
    if (zoomStartX !== null) {
      const r = plotRect();
      const rect = canvas.getBoundingClientRect();
      zoomCurX = Math.max(0, Math.min(r.w, ev.clientX - rect.left - r.x));
      draw();
      return;
    }
    if (dragLastX === null) return;
    const r = plotRect();
    const rect = canvas.getBoundingClientRect();
    const localX = ev.clientX - rect.left - r.x;
    const localY = ev.clientY - rect.top - r.y;
    if (dragCursor === 1 || dragCursor === 2) {
      const pct = Math.max(0, Math.min(100, (localX / r.w) * 100));
      dispatch('cursormove', { which: dragCursor, pct });
      dragLastX = ev.clientX;
      return;
    }
    if (dragCursor === 3 || dragCursor === 4) {
      // Convert pixel Y back to mV (in CH A's scale).
      const halfA = vdivMvA > 0 ? vdivMvA * (GRID_ROWS / 2) : rangeMvA;
      const sc = (r.h / 2) / halfA;
      const mv = -(localY - r.h / 2) / sc - offsetMvA;
      dispatch('ycursormove', { which: dragCursor === 3 ? 1 : 2, mv });
      dragLastX = ev.clientX;
      return;
    }
    const dxPx = ev.clientX - dragLastX;
    dragLastX = ev.clientX;
    if (r.w > 0 && dxPx !== 0) {
      dispatch('pan', { dxFrac: dxPx / r.w });
    }
  }

  function onPointerUp(ev) {
    if (zoomStartX !== null) {
      const r = plotRect();
      const x0 = Math.min(zoomStartX, zoomCurX);
      const x1 = Math.max(zoomStartX, zoomCurX);
      zoomStartX = null; zoomCurX = null;
      try { canvas.releasePointerCapture(ev.pointerId); } catch (_) {}
      canvas.style.cursor = 'crosshair';
      // Minimum width: 1 % of plot to avoid firing on accidental clicks.
      if (r.w > 0 && (x1 - x0) / r.w >= 0.01) {
        dispatch('zoomTo', {
          startFrac: Math.max(0, x0 / r.w),
          endFrac:   Math.min(1, x1 / r.w),
        });
      }
      draw();
      return;
    }
    if (dragLastX === null) return;
    dragLastX = null;
    dragCursor = 0;
    try { canvas.releasePointerCapture(ev.pointerId); } catch (_) {}
    canvas.style.cursor = 'crosshair';
  }

  function onContextMenu(ev) { ev.preventDefault(); }

  /* Reactive redraw when any prop changes */
  $: if (ctx) {
    samplesA; samplesB; samplesM; rangeMvA; rangeMvB; startTimeNs; spanNs;
    channelAEnabled; channelBEnabled; mathLabel;
    xyMode; cursorsOn; cursor1Pct; cursor2Pct; persistenceOn;
    offsetMvA; offsetMvB; vdivMvA; vdivMvB;
    yCursorsOn; yCursor1Mv; yCursor2Mv;
    annotations;
    draw();
  }

  onMount(() => {
    ctx = canvas.getContext('2d');
    resizeCanvas();
    window.addEventListener('resize', resizeCanvas);
  });

  onDestroy(() => {
    window.removeEventListener('resize', resizeCanvas);
  });

  // External hook: snapshot the current canvas as a PNG data URL.
  export function snapshotDataURL() {
    return canvas ? canvas.toDataURL('image/png') : null;
  }
</script>

<div class="canvas-container">
  <canvas
    bind:this={canvas}
    on:wheel={onWheel}
    on:pointerdown={onPointerDown}
    on:pointermove={onPointerMove}
    on:pointerup={onPointerUp}
    on:pointercancel={onPointerUp}
    on:contextmenu={onContextMenu}
    title="Drag = pan · Shift+drag or right-drag = box zoom · Wheel = zoom · ΔtΔV cursors drag when enabled"
  ></canvas>
</div>

<style>
  .canvas-container {
    width: 100%;
    height: 100%;
    min-height: 300px;
    position: relative;
  }
  canvas {
    display: block;
    width: 100%;
    height: 100%;
    cursor: crosshair;
    touch-action: none;
  }
</style>
