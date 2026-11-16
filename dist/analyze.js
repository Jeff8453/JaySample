// Local heuristic analysis: spectral chroma + onset autocorrelation. Estimates are editable.
self.onmessage = ({ data: { samples: d, rate } }) => {
  const hop = 110,
    env = [];
  let energy = 0;
  for (let i = 0; i < d.length; i += hop) {
    let e = 0;
    for (let j = i; j < Math.min(d.length, i + hop); j++) e += d[j] * d[j];
    env.push(Math.sqrt(e / hop));
    energy += e;
  }
  const onset = env.map((v, i) => Math.max(0, v - (env[i - 1] || 0))),
    fps = rate / hop;
  let best = 0,
    bpm = null;
  if (d.length / rate >= 2) {
    for (let b = 60; b <= 180; b += 0.5) {
      const lag = Math.round((fps * 60) / b);
      let corr = 0,
        norm = 0;
      for (let i = lag; i < onset.length; i++) {
        corr += onset[i] * onset[i - lag];
        norm += onset[i] * onset[i];
      }
      const score = corr / (norm || 1);
      if (score > best) {
        best = score;
        bpm = b;
      }
    }
    if (best < 0.08) bpm = null;
  }
  // Reject nearly stationary tones/chords instead of labelling their beating as a confident tempo.
  const smooth = [];
  for (let i = 0; i < env.length; i += 5)
    smooth.push(
      env.slice(i, i + 5).reduce((a, b) => a + b, 0) /
        Math.min(5, env.length - i),
    );
  const mean = smooth.reduce((a, b) => a + b, 0) / (smooth.length || 1),
    variation =
      Math.sqrt(
        smooth.reduce((a, b) => a + (b - mean) ** 2, 0) / (smooth.length || 1),
      ) / (mean || 1);
  const threshold = Math.max(...onset) * 0.28,
    peaks = [];
  for (let i = 1; i < onset.length - 1; i++)
    if (
      onset[i] > threshold &&
      onset[i] >= onset[i - 1] &&
      onset[i] > onset[i + 1]
    ) {
      if (!peaks.length || i - peaks.at(-1) > fps * 0.15) peaks.push(i);
    }
  if (peaks.length >= 5) {
    const intervals = peaks.slice(1).map((p, i) => p - peaks[i]),
      avg = intervals.reduce((a, b) => a + b, 0) / intervals.length,
      cv =
        Math.sqrt(
          intervals.reduce((a, b) => a + (b - avg) ** 2, 0) / intervals.length,
        ) / avg;
    if (cv < 0.07) {
      const n = peaks.length,
        xmean = (n - 1) / 2,
        ymean = peaks.reduce((a, b) => a + b, 0) / n;
      let numerator = 0,
        denominator = 0;
      for (let i = 0; i < n; i++) {
        numerator += (i - xmean) * (peaks[i] - ymean);
        denominator += (i - xmean) ** 2;
      }
      const estimate = (60 * fps) / (numerator / denominator);
      if (estimate >= 60 && estimate <= 180) {
        bpm = Math.round(estimate * 10) / 10;
        best = Math.max(best, 0.6);
      }
    }
  }
  if (variation < 0.09 || energy / d.length < 1e-7) bpm = null;
  const chroma = new Float64Array(12),
    N = 8192;
  function fft(re, im) {
    for (let i = 1, j = 0; i < N; i++) {
      let bit = N >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        [re[i], re[j]] = [re[j], re[i]];
        [im[i], im[j]] = [im[j], im[i]];
      }
    }
    for (let len = 2; len <= N; len <<= 1) {
      const angle = (-2 * Math.PI) / len;
      for (let i = 0; i < N; i += len) {
        let wr = 1,
          wi = 0;
        for (let j = 0; j < len / 2; j++) {
          let u = i + j,
            v = u + len / 2,
            tr = wr * re[v] - wi * im[v],
            ti = wr * im[v] + wi * re[v];
          re[v] = re[u] - tr;
          im[v] = im[u] - ti;
          re[u] += tr;
          im[u] += ti;
          const next = wr * Math.cos(angle) - wi * Math.sin(angle);
          wi = wr * Math.sin(angle) + wi * Math.cos(angle);
          wr = next;
        }
      }
    }
  }
  for (let at = 0; at + N < d.length; at += Math.floor(rate * 0.4)) {
    const re = new Float64Array(N),
      im = new Float64Array(N);
    for (let j = 0; j < N; j++)
      re[j] = d[at + j] * (0.5 - 0.5 * Math.cos((2 * Math.PI * j) / (N - 1)));
    fft(re, im);
    for (let k = 20; k < N / 2 - 1; k++) {
      const power = re[k] * re[k] + im[k] * im[k];
      if (
        power < re[k - 1] * re[k - 1] + im[k - 1] * im[k - 1] ||
        power < re[k + 1] * re[k + 1] + im[k + 1] * im[k + 1]
      )
        continue;
      const left = Math.log(
          re[k - 1] * re[k - 1] + im[k - 1] * im[k - 1] + 1e-20,
        ),
        center = Math.log(power + 1e-20),
        right = Math.log(re[k + 1] * re[k + 1] + im[k + 1] * im[k + 1] + 1e-20),
        delta = Math.max(
          -0.5,
          Math.min(
            0.5,
            (0.5 * (left - right)) / (left - 2 * center + right || 1),
          ),
        );
      const midi = 69 + 12 * Math.log2(((k + delta) * rate) / N / 440),
        note = Math.round(midi);
      if (note < 36 || note > 95 || Math.abs(midi - note) > 0.35) continue;
      chroma[note % 12] += Math.sqrt(power);
    }
  }
  const profiles = [
    [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88],
    [6.33, 2.68, 3.52, 5.38, 2.6, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17],
  ];
  function correlation(a, b) {
    let ma = a.reduce((s, v) => s + v, 0) / 12,
      mb = b.reduce((s, v) => s + v, 0) / 12,
      num = 0,
      da = 0,
      db = 0;
    for (let i = 0; i < 12; i++) {
      num += (a[i] - ma) * (b[i] - mb);
      da += (a[i] - ma) ** 2;
      db += (b[i] - mb) ** 2;
    }
    return num / Math.sqrt(da * db || 1);
  }
  const scores = [];
  for (let mode = 0; mode < 2; mode++)
    for (let root = 0; root < 12; root++)
      scores.push({
        key: root + mode * 12,
        score: correlation(
          [...chroma],
          Array.from(
            { length: 12 },
            (_, i) => profiles[mode][(i - root + 12) % 12],
          ),
        ),
      });
  scores.sort((a, b) => b.score - a.score);
  const spread = Math.max(...chroma) / (chroma.reduce((a, b) => a + b, 0) || 1),
    gap = scores[0].score - scores[1].score;
  const key =
    energy / d.length < 1e-7 ||
    scores[0].score < 0.7 ||
    spread > 0.65 ||
    [...chroma].filter((v) => v > Math.max(...chroma) * 0.15).length < 3
      ? null
      : scores[0].key;
  self.postMessage({
    bpm,
    key,
    tempoConfidence:
      bpm === null
        ? "low"
        : best > 0.45
          ? "higher"
          : best > 0.18
            ? "moderate"
            : "low",
    alternatives: bpm
      ? [bpm / 2, bpm * 2].filter((n) => n >= 30 && n <= 300)
      : [],
    confidence:
      key === null
        ? "low"
        : gap > 0.12 && scores[0].score > 0.7
          ? "higher"
          : "low",
  });
};
