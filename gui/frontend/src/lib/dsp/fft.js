/**
 * Radix-2 FFT with Hann window. Magnitude is in dB, normalised so a pure
 * full-scale sinusoid reads ≈ 0 dB.
 *
 * @param {ArrayLike<number>} samples
 * @param {number} fs   sample rate in Hz
 * @returns {{freq: Float64Array, mag: Float64Array, n: number, fs: number} | null}
 */
export function fftMag(samples, fs) {
  if (!samples || samples.length < 16) return null;
  // Pick N = largest power of 2 <= samples.length, cap at 8192 (plenty for scope).
  let N = 1;
  while (N * 2 <= samples.length && N * 2 <= 8192) N *= 2;
  if (N < 16) return null;

  const re = new Float64Array(N);
  const im = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    const w = 0.5 * (1 - Math.cos(2 * Math.PI * i / (N - 1)));
    re[i] = samples[i] * w;
  }

  // Bit-reverse permutation
  for (let i = 1, j = 0; i < N; i++) {
    let bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }

  // Butterflies
  for (let size = 2; size <= N; size *= 2) {
    const half = size / 2;
    const ang = -2 * Math.PI / size;
    for (let i = 0; i < N; i += size) {
      for (let k = 0; k < half; k++) {
        const c = Math.cos(ang * k), s = Math.sin(ang * k);
        const tr = c * re[i + k + half] - s * im[i + k + half];
        const ti = s * re[i + k + half] + c * im[i + k + half];
        re[i + k + half] = re[i + k] - tr;
        im[i + k + half] = im[i + k] - ti;
        re[i + k] += tr;
        im[i + k] += ti;
      }
    }
  }

  const M = N / 2;
  const mag = new Float64Array(M);
  const freq = new Float64Array(M);
  const norm = 2 / N;
  for (let i = 0; i < M; i++) {
    const m = Math.sqrt(re[i] * re[i] + im[i] * im[i]) * norm;
    mag[i] = 20 * Math.log10(Math.max(m, 1e-6));
    freq[i] = i * fs / N;
  }
  return { freq, mag, n: N, fs };
}
