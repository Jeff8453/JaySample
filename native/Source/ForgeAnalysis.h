#pragma once
// ForgeAnalysis.h — key / BPM detection, transient detection, Find Samples, slicing and snapping.
// No JUCE: unit-tested on its own. Key numbers match the browser version: 0-11 major (C..B), 12-23 minor.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numeric>
#include <random>
#include <vector>

namespace forge {

// in-place radix-2 FFT (size = power of two)
inline void fft(std::vector<std::complex<double>> &a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2 * 3.141592653589793 / double(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1);
      for (size_t j = 0; j < len / 2; j++) {
        const auto u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v;
        a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

inline std::vector<float> mixToMono(const std::array<std::vector<float>, 2> &audio) {
  std::vector<float> m(audio[0].size());
  for (size_t i = 0; i < m.size(); i++) m[i] = .5f * (audio[0][i] + audio[1][i]);
  return m;
}

struct KeyBpm {
  double bpm = 0;      // 0 = unknown
  int key = -1;        // -1 = unknown, 0-11 major, 12-23 minor
  bool bpmConfident = false, keyConfident = false;
};

inline const char *keyName(int key) {
  static const char *names[24] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
                                   "Cm", "C#m", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "G#m", "Am", "Bbm", "Bm" };
  return key >= 0 && key < 24 ? names[key] : "?";
}

// Same heuristic as the browser version: onset autocorrelation for tempo, spectral chroma + key profiles for key.
inline KeyBpm analyzeKeyBpm(const std::vector<float> &d, double rate) {
  KeyBpm out;
  if (d.empty() || rate <= 0) return out;
  const int hop = std::max(1, int(std::round(110.0 * rate / 44100.0)));
  std::vector<double> env;
  double energy = 0;
  for (size_t i = 0; i < d.size(); i += size_t(hop)) {
    double e = 0;
    const size_t end = std::min(d.size(), i + size_t(hop));
    for (size_t j = i; j < end; j++) e += double(d[j]) * d[j];
    env.push_back(std::sqrt(e / hop));
    energy += e;
  }
  std::vector<double> onset(env.size());
  for (size_t i = 0; i < env.size(); i++) onset[i] = std::max(0.0, env[i] - (i ? env[i - 1] : 0.0));
  const double fps = rate / hop;
  double best = 0, bpm = 0;
  if (double(d.size()) / rate >= 2) {
    for (double b = 60; b <= 180; b += .5) {
      const size_t lag = size_t(std::round(fps * 60 / b));
      double corr = 0, norm = 0;
      for (size_t i = lag; i < onset.size(); i++) { corr += onset[i] * onset[i - lag]; norm += onset[i] * onset[i]; }
      const double score = corr / (norm > 0 ? norm : 1);
      if (score > best) { best = score; bpm = b; }
    }
    if (best < .08) bpm = 0;
  }
  // stationary tones / chords: no confident tempo
  std::vector<double> smooth;
  for (size_t i = 0; i < env.size(); i += 5) {
    double s = 0; size_t c = 0;
    for (size_t j = i; j < std::min(env.size(), i + 5); j++, c++) s += env[j];
    smooth.push_back(s / double(c));
  }
  double mean = smooth.empty() ? 0 : std::accumulate(smooth.begin(), smooth.end(), 0.0) / double(smooth.size()), var = 0;
  for (double v : smooth) var += (v - mean) * (v - mean);
  const double variation = smooth.empty() || mean <= 0 ? 0 : std::sqrt(var / double(smooth.size())) / mean;
  // very steady beats: fit the onset peaks directly (more precise)
  double maxOn = 0; for (double v : onset) maxOn = std::max(maxOn, v);
  std::vector<size_t> peaks;
  for (size_t i = 1; i + 1 < onset.size(); i++)
    if (onset[i] > maxOn * .28 && onset[i] >= onset[i - 1] && onset[i] > onset[i + 1])
      if (peaks.empty() || double(i - peaks.back()) > fps * .15) peaks.push_back(i);
  if (peaks.size() >= 5) {
    std::vector<double> iv;
    for (size_t i = 1; i < peaks.size(); i++) iv.push_back(double(peaks[i] - peaks[i - 1]));
    const double avg = std::accumulate(iv.begin(), iv.end(), 0.0) / double(iv.size());
    double v2 = 0; for (double x : iv) v2 += (x - avg) * (x - avg);
    if (std::sqrt(v2 / double(iv.size())) / avg < .07) {
      const double n = double(peaks.size()), xm = (n - 1) / 2;
      double ym = 0; for (auto p : peaks) ym += double(p); ym /= n;
      double num = 0, den = 0;
      for (size_t i = 0; i < peaks.size(); i++) { num += (double(i) - xm) * (double(peaks[i]) - ym); den += (double(i) - xm) * (double(i) - xm); }
      const double est = 60 * fps / (num / den);
      if (est >= 60 && est <= 180) { bpm = std::round(est * 10) / 10; best = std::max(best, .6); }
    }
  }
  if (variation < .09 || energy / double(d.size()) < 1e-7) bpm = 0;
  out.bpm = bpm;
  out.bpmConfident = bpm > 0 && best > .18;

  // key: peak-picked spectral chroma
  std::array<double, 12> chroma {};
  const size_t N = 8192;
  std::vector<std::complex<double>> buf(N);
  const size_t stepAt = std::max<size_t>(1, size_t(rate * .4));
  for (size_t at = 0; at + N < d.size(); at += stepAt) {
    for (size_t j = 0; j < N; j++) buf[j] = d[at + j] * (.5 - .5 * std::cos(2 * 3.141592653589793 * double(j) / double(N - 1)));
    fft(buf);
    for (size_t k = 20; k < N / 2 - 1; k++) {
      const double p = std::norm(buf[k]), pl = std::norm(buf[k - 1]), pr = std::norm(buf[k + 1]);
      if (p < pl || p < pr) continue;
      const double l = std::log(pl + 1e-20), c = std::log(p + 1e-20), r = std::log(pr + 1e-20);
      const double den = l - 2 * c + r;
      const double delta = std::max(-.5, std::min(.5, .5 * (l - r) / (den != 0 ? den : 1)));
      const double midi = 69 + 12 * std::log2((double(k) + delta) * rate / double(N) / 440);
      const int note = int(std::round(midi));
      if (note < 36 || note > 95 || std::abs(midi - note) > .35) continue;
      chroma[size_t(note % 12)] += std::sqrt(p);
    }
  }
  static const double prof[2][12] = { { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 },
                                      { 6.33, 2.68, 3.52, 5.38, 2.6, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 } };
  auto corr = [](const std::array<double, 12> &a, const double *b, int root) {
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; i++) { ma += a[size_t(i)]; mb += b[(i - root + 12) % 12]; }
    ma /= 12; mb /= 12;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; i++) {
      const double x = a[size_t(i)] - ma, y = b[(i - root + 12) % 12] - mb;
      num += x * y; da += x * x; db += y * y;
    }
    return num / std::sqrt(da * db > 0 ? da * db : 1);
  };
  double s1 = -2, s2 = -2; int k1 = -1;
  for (int mode = 0; mode < 2; mode++)
    for (int root = 0; root < 12; root++) {
      const double sc = corr(chroma, prof[mode], root);
      if (sc > s1) { s2 = s1; s1 = sc; k1 = root + mode * 12; }
      else if (sc > s2) s2 = sc;
    }
  double total = 0, mx = 0; int strong = 0;
  for (double v : chroma) { total += v; mx = std::max(mx, v); }
  for (double v : chroma) if (v > mx * .15) strong++;
  const double spread = total > 0 ? mx / total : 1;
  if (!(energy / double(d.size()) < 1e-7 || s1 < .7 || spread > .65 || strong < 3)) {
    out.key = k1;
    out.keyConfident = s1 - s2 > .12;
  }
  return out;
}

// ---------------------------------------------------------------- transients
struct Onset { double time = 0; float strength = 0; };

// spectral-flux onsets (1024 window, hop 256 @ 44.1k), adaptive threshold; times snapped just before the hit
inline std::vector<Onset> detectOnsets(const std::vector<float> &input, double rate) {
  std::vector<Onset> out;
  if (input.size() < 2048 || rate <= 0) return out;
  // silence in front, so a hit right at the start of the file is found too
  const size_t pad = 2048;
  std::vector<float> d(pad, 0.f);
  d.insert(d.end(), input.begin(), input.end());
  const size_t N = 1024, hop = std::max<size_t>(64, size_t(256 * rate / 44100));
  const size_t frames = (d.size() - N) / hop + 1;
  std::vector<double> flux(frames, 0), prev(N / 2, 0), mag(N / 2);
  std::vector<std::complex<double>> buf(N);
  std::vector<double> win(N);
  for (size_t j = 0; j < N; j++) win[j] = .5 - .5 * std::cos(2 * 3.141592653589793 * double(j) / double(N - 1));
  for (size_t f = 0; f < frames; f++) {
    const size_t at = f * hop;
    for (size_t j = 0; j < N; j++) buf[j] = d[at + j] * win[j];
    fft(buf);
    double s = 0;
    for (size_t k = 1; k < N / 2; k++) {
      mag[k] = std::log1p(100 * std::abs(buf[k]));
      s += std::max(0.0, mag[k] - prev[k]);
    }
    flux[f] = f ? s : 0;
    std::swap(prev, mag);
  }
  // adaptive threshold: local median + a bit of the global mean
  const double gmean = std::accumulate(flux.begin(), flux.end(), 0.0) / double(frames);
  const size_t w = 8;
  std::vector<double> tmp;
  for (size_t f = 1; f + 1 < frames; f++) {
    if (flux[f] < flux[f - 1] || flux[f] < flux[f + 1]) continue;
    tmp.assign(flux.begin() + long(f > w ? f - w : 0), flux.begin() + long(std::min(frames, f + w + 1)));
    std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
    const double thr = tmp[tmp.size() / 2] * 1.4 + gmean * .3;
    if (flux[f] <= thr) continue;
    // the hit is in the newest hop of this window: find its attack (first block well above the floor before it)
    const size_t blk = 32;
    const size_t from = f * hop + N > 2 * hop ? f * hop + N - 2 * hop : 0, to = std::min(d.size(), f * hop + N + hop);
    std::vector<double> e;
    for (size_t p = from; p + blk <= to; p += blk) { double s = 0; for (size_t q = p; q < p + blk; q++) s += double(d[q]) * d[q]; e.push_back(s / blk); }
    if (e.empty()) continue;
    const size_t pk = size_t(std::max_element(e.begin(), e.end()) - e.begin());
    double floorE = e[pk];
    for (size_t i = 0; i <= pk; i++) floorE = std::min(floorE, e[i]);
    const double level = floorE + .1 * (e[pk] - floorE);
    size_t first = pk;
    for (size_t i = 0; i <= pk; i++) if (e[i] >= level) { first = i; break; }
    const size_t atk = from + first * blk;
    const size_t at = atk > size_t(.001 * rate) ? atk - size_t(.001 * rate) : 0;   // 1 ms before: keep the whole attack
    // strength: how much louder it gets (30 ms after vs 30 ms before)
    auto rms = [&](size_t a, size_t b) { double s = 0; b = std::min(b, d.size()); for (size_t q = a; q < b; q++) s += double(d[q]) * d[q]; return b > a ? std::sqrt(s / double(b - a)) : 0.0; };
    const size_t w30 = size_t(.03 * rate);
    const double rise = rms(atk, atk + w30) - rms(atk > w30 ? atk - w30 : 0, atk);
    if (rise <= 1e-5) continue;
    Onset o; o.time = double(at > pad ? at - pad : 0) / rate; o.strength = float(rise);
    if (!out.empty() && o.time - out.back().time < .05) {   // one hit, keep the stronger
      if (o.strength > out.back().strength) out.back() = o;
      continue;
    }
    out.push_back(o);
  }
  return out;
}

struct Slice { double start = 0, end = 0; };

// Find Samples: up to `count` cues on the strongest hits. sensitivity 0..1 (more = weaker hits count).
// seed 0 = the strongest hits; another seed = a different pick among the good ones (re-roll).
inline std::vector<Slice> findSamples(const std::vector<Onset> &onsets, double length, int count,
                                      float sensitivity, uint32_t seed, double maxLen = 4.0) {
  std::vector<Slice> out;
  if (onsets.empty() || length <= 0 || count <= 0) return out;
  float top = 0; for (const auto &o : onsets) top = std::max(top, o.strength);
  const float floorS = top * (.6f - .55f * std::clamp(sensitivity, 0.f, 1.f));
  std::vector<Onset> cand;
  for (const auto &o : onsets) if (o.strength >= floorS) cand.push_back(o);
  if (cand.empty()) cand.push_back(*std::max_element(onsets.begin(), onsets.end(), [](auto &a, auto &b) { return a.strength < b.strength; }));
  std::vector<Onset> chosen;
  const double minGap = .12;
  auto tooClose = [&](double t) { for (const auto &c : chosen) if (std::abs(c.time - t) < minGap) return true; return false; };
  if (seed == 0) {
    std::sort(cand.begin(), cand.end(), [](auto &a, auto &b) { return a.strength > b.strength; });
    for (const auto &o : cand) { if ((int) chosen.size() >= count) break; if (! tooClose(o.time)) chosen.push_back(o); }
  } else {
    std::mt19937 rng(seed);
    std::vector<Onset> pool = cand;
    while ((int) chosen.size() < count && ! pool.empty()) {
      double total = 0; for (const auto &o : pool) total += std::sqrt(double(o.strength)) + 1e-9;   // flatter odds: re-roll explores
      std::uniform_real_distribution<double> u(0, total);
      double r = u(rng); size_t i = 0;
      for (; i + 1 < pool.size(); i++) { r -= std::sqrt(double(pool[i].strength)) + 1e-9; if (r <= 0) break; }
      if (! tooClose(pool[i].time)) chosen.push_back(pool[i]);
      pool.erase(pool.begin() + long(i));
    }
  }
  std::sort(chosen.begin(), chosen.end(), [](auto &a, auto &b) { return a.time < b.time; });
  // each cue plays until the next detected hit (any strength) or maxLen
  for (const auto &c : chosen) {
    double end = std::min(length, c.time + maxLen);
    for (const auto &o : onsets) if (o.time > c.time + .08) { end = std::min(end, o.time); break; }
    if (end - c.time >= .03) out.push_back({ c.time, end });
  }
  return out;
}

inline std::vector<Slice> evenSlices(int count, double from, double to) {
  std::vector<Slice> out;
  if (count <= 0 || to <= from) return out;
  const double len = (to - from) / count;
  for (int i = 0; i < count; i++) out.push_back({ from + i * len, from + (i + 1) * len });
  return out;
}

// nearest zero crossing (rising or falling) within 5 ms
inline double snapToZero(const std::vector<float> &d, double rate, double t) {
  const long c = long(std::round(t * rate)), r = long(.005 * rate);
  long best = c, bestD = r + 1;
  for (long i = std::max(1L, c - r); i <= std::min(long(d.size()) - 1, c + r); i++)
    if ((d[size_t(i - 1)] <= 0 && d[size_t(i)] > 0) || (d[size_t(i - 1)] >= 0 && d[size_t(i)] < 0))
      if (std::abs(i - c) < bestD) { bestD = std::abs(i - c); best = i; }
  return std::max(0.0, double(best) / rate);
}

// nearest 1/16 note on the sample's own beat grid
inline double snapToBeat(double t, double bpm, double offset = 0) {
  if (bpm <= 0) return t;
  const double g = 60.0 / bpm / 4;
  return std::max(0.0, offset + std::round((t - offset) / g) * g);
}

// min/max overview: one pair per `block` samples (mono mix)
struct Peaks { std::vector<float> lo, hi; int block = 256; };
inline Peaks makePeaks(const std::vector<float> &d, int block = 256) {
  Peaks p; p.block = block;
  for (size_t i = 0; i < d.size(); i += size_t(block)) {
    float lo = 0, hi = 0;
    for (size_t j = i; j < std::min(d.size(), i + size_t(block)); j++) { lo = std::min(lo, d[j]); hi = std::max(hi, d[j]); }
    p.lo.push_back(lo); p.hi.push_back(hi);
  }
  return p;
}

} // namespace forge
