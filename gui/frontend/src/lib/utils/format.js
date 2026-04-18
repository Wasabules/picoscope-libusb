export function fmtMv(v) {
  if (v === null || v === undefined) return '--';
  if (Math.abs(v) >= 1000) return (v / 1000).toFixed(2) + ' V';
  return v.toFixed(1) + ' mV';
}

export function fmtHz(v) {
  if (!v || !isFinite(v) || v <= 0) return '--';
  if (v >= 1e6) return (v / 1e6).toFixed(2) + ' MHz';
  if (v >= 1e3) return (v / 1e3).toFixed(2) + ' kHz';
  return v.toFixed(1) + ' Hz';
}

export function fmtRate(v) {
  if (!v) return '--';
  if (v >= 1e6) return (v / 1e6).toFixed(1) + ' MS/s';
  if (v >= 1e3) return (v / 1e3).toFixed(0) + ' kS/s';
  return v.toFixed(0) + ' S/s';
}

export function fmtCount(v) {
  if (!v) return '0';
  if (v >= 1e6) return (v / 1e6).toFixed(1) + 'M';
  if (v >= 1e3) return (v / 1e3).toFixed(1) + 'k';
  return String(v);
}

export function fmtTime(ns) {
  const abs = Math.abs(ns);
  if (abs < 1e3)  return ns.toFixed(0) + ' ns';
  if (abs < 1e6)  return (ns / 1e3).toFixed(abs < 1e4 ? 2 : 1) + ' µs';
  if (abs < 1e9)  return (ns / 1e6).toFixed(abs < 1e7 ? 2 : 1) + ' ms';
  return (ns / 1e9).toFixed(abs < 1e10 ? 2 : 1) + ' s';
}

export function fmtMeas(fmt, v) {
  if (v == null || !isFinite(v)) return '—';
  if (fmt === 'mv')   return fmtMv(v);
  if (fmt === 'hz')   return fmtHz(v);
  if (fmt === 'time') return fmtTime(v);
  if (fmt === 'pct')  return v.toFixed(1) + '%';
  return String(v);
}
