// JaySample native DSP tests. Build: c++ -std=c++17 -O2 -I Source -I ThirdParty tests/dsp_test.cpp
#include "ForgeDSP.h"
#include "ForgeAnalysis.h"
#include <cstdio>
#include <iostream>

static int failures = 0;
static void check(bool ok, const char *what, double value) {
  std::printf("%s %-78s (%.4f)\n", ok ? "ok  " : "FAIL", what, value);
  if (!ok) failures++;
}

// a drum-like hit: decaying noise + a low thump
static void addHit(std::vector<float> &d, double rate, double t, float amp, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1, 1);
  const size_t at = size_t(t * rate), len = size_t(.18 * rate);
  for (size_t i = 0; i < len && at + i < d.size(); i++) {
    const float env = std::exp(-float(i) / float(.03 * rate));
    d[at + i] += amp * env * (.6f * u(rng) + .4f * std::sin(2 * 3.14159265f * 90.f * float(i) / float(rate)));
  }
}

int main() {
  constexpr int rate = 44100;
  // ---- original: independent pitch / tempo, short cue, release, resampling
  {
    std::array<std::vector<float>, 2> source;
    for (auto &ch : source) {
      ch.resize(rate * 2);
      for (size_t i = 0; i < ch.size(); i++) ch[i] = .2f * std::sin(2 * 3.141592653589793 * 261.6256 * i / rate);
    }
    forge::Cue cue; cue.present = true; cue.end = 2; cue.attack = 0;
    auto rendered = forge::render(source, rate, cue, 1.25, 12, false);
    int crossings = 0;
    for (size_t i = rate / 5; i < rendered->size() - rate / 5; i++)
      if (rendered->data[0][i] < 0 && rendered->data[0][i + 1] >= 0) crossings++;
    const double hz = crossings / 1.2;
    check(rendered->size() == 70560 && std::abs(hz - 523.25) < 3, "render: +12 st and 1.25x tempo independently (C4 -> C5, 1.6 s)", hz);
    forge::Voice v;
    v.start(rendered.get(), 48000, 36, 1, .8f);
    float l = 0, r = 0;
    bool finite = true;
    for (int i = 0; i < 4000; i++) { l = r = 0; v.tick(l, r, 48000); finite = finite && std::isfinite(l); }
    v.release();
    for (int i = 0; i < 10000; i++) { l = r = 0; v.tick(l, r, 48000); }
    cue.end = .004;
    auto tiny = forge::render(source, rate, cue, 1.3, 7, false);
    check(finite && !v.active() && tiny->size() > 0, "voice plays finite, release stops it; a 4 ms cue renders", 0);
  }

  // ---- click-free start and stop
  {
    auto clip = std::make_shared<forge::Clip>();
    clip->sampleRate = rate;
    clip->data[0].assign(rate, 1.f);
    clip->data[1].assign(rate, 1.f);
    clip->mode = 2;   // loop: no trigger tail
    forge::Voice v;
    v.start(clip.get(), rate, 36, 1, 1);
    float l = 0, r = 0;
    v.tick(l, r, rate);
    const float first = l;
    for (int i = 0; i < 200; i++) { l = r = 0; v.tick(l, r, rate); }
    const float settled = l;
    v.kill(rate, 8);
    float prev = settled, maxJump = 0;
    int n = 0;
    while (v.active() && n < rate) { l = r = 0; v.tick(l, r, rate); maxJump = std::max(maxJump, std::abs(l - prev)); prev = l; n++; }
    const double ms = n * 1000.0 / rate;
    check(first < .02f && settled > .99f, "voice start fades in over ~2 ms (no click)", first);
    check(!v.active() && ms > 6 && ms < 10 && maxJump < .01f, "kill (steal / choke / bank swap / Stop) fades out in ~8 ms, no jumps", ms);
  }

  // ---- master bus: limiter only above -1 dBFS; low / high cut
  {
    forge::MasterBus bus; bus.prepare(rate);
    float l = .5f, r = -.5f; bus.process(l, r);
    const bool clean = l == .5f && r == -.5f;
    float peak = 0;
    for (int i = 0; i < rate / 10; i++) { float a = 2.f * std::sin(i * .05f), b = a; bus.process(a, b); peak = std::max(peak, std::abs(a)); }
    check(clean && peak <= .8915f, "limiter: untouched at -6 dB, holds +6 dB peaks at -1 dBFS", peak);
    auto level = [&](float hz, float low, float high) {
      forge::MasterBus b; b.prepare(rate); b.setFilters(low, high);
      double s = 0;
      for (int i = 0; i < rate; i++) {
        float a = .3f * std::sin(2 * 3.14159265f * hz * i / rate), c = a;
        b.process(a, c);
        if (i > rate / 2) s += double(a) * a;
      }
      return std::sqrt(s / (rate / 2));
    };
    const double hc = 20 * std::log10(level(8000, 20, 1000) / level(8000, 20, 20000));
    const double lc = 20 * std::log10(level(50, 300, 20000) / level(50, 20, 20000));
    const double pass = 20 * std::log10(level(1000, 20, 20000) / (.3 / std::sqrt(2.0)));
    check(hc < -20 && lc < -12 && std::abs(pass) < .1, "high cut 1 kHz removes 8 kHz; low cut 300 Hz removes 50 Hz; off = flat", hc);
  }

  // ---- transients: hits found where they are
  {
    std::vector<float> d(size_t(rate * 4), 0.f);
    const double times[] = { .5, 1.0, 1.37, 2.1, 2.62, 3.3 };
    const float amps[] = { .9f, .4f, .7f, .25f, .8f, .5f };
    for (int i = 0; i < 6; i++) addHit(d, rate, times[i], amps[i], uint32_t(i + 1));
    const auto on = forge::detectOnsets(d, rate);
    int found = 0; double worst = 0;
    for (double t : times)
      for (const auto &o : on)
        if (std::abs(o.time - t) < .02) { found++; worst = std::max(worst, std::abs(o.time - t)); break; }
    check(found == 6 && on.size() == 6, "onsets: all 6 hits found, nothing extra, each within 20 ms", worst * 1000);
    bool before = true;
    for (double t : times) for (const auto &o : on) if (std::abs(o.time - t) < .02) before = before && o.time <= t + .003;
    check(before, "onsets start at (not after) the hit, so cues don't clip the attack", 0);

    // Find Samples: the strongest 4, in time order, each ending at the next hit
    const auto s = forge::findSamples(on, 4.0, 4, .5f, 0);
    const bool strongest = s.size() == 4 && std::abs(s[0].start - .5) < .02 && std::abs(s[1].start - 1.37) < .02 &&
                           std::abs(s[2].start - 2.62) < .02 && std::abs(s[3].start - 3.3) < .02;
    const bool ends = s.size() == 4 && std::abs(s[0].end - 1.0) < .02 && std::abs(s[2].end - 3.3) < .02;
    check(strongest && ends, "Find Samples: picks the 4 strongest hits in time order; each ends at the next hit", s.size());
    int differentSets = 0; bool allValid = true;
    for (uint32_t seed = 1; seed <= 10; seed++) {
      const auto re = forge::findSamples(on, 4.0, 4, 1.f, seed);
      allValid = allValid && re.size() == 4;
      bool differs = false;
      for (size_t i = 0; i < re.size() && i < s.size(); i++) if (std::abs(re[i].start - s[i].start) > .01) differs = true;
      for (size_t i = 1; i < re.size(); i++) allValid = allValid && re[i].start > re[i - 1].start;
      differentSets += differs ? 1 : 0;
    }
    check(allValid && differentSets >= 5, "Re-roll: other seeds give different valid sets (4 cues, in order)", differentSets);
    const auto all = forge::findSamples(on, 4.0, 16, 1.f, 0);
    check(all.size() == 6, "asking for 16 with 6 hits gives 6 cues (no duplicates)", all.size());
  }

  // ---- tempo: a 92 BPM drum loop
  {
    std::vector<float> d(size_t(rate * 12), 0.f);
    const double beat = 60.0 / 92;
    for (int i = 0; i * beat < 11.8; i++) addHit(d, rate, i * beat, i % 4 == 0 ? .9f : .5f, uint32_t(i + 10));
    const auto kb = forge::analyzeKeyBpm(d, rate);
    check(std::abs(kb.bpm - 92) < 1.0, "BPM: a 92 BPM loop reads as 92", kb.bpm);
  }
  // ---- key: an A minor progression (Am Dm Em Am with the tonic in the bass)
  {
    std::vector<float> d(size_t(rate * 8), 0.f);
    const int chords[4][4] = { { 45, 57, 60, 64 }, { 50, 57, 62, 65 }, { 52, 59, 64, 67 }, { 45, 57, 60, 64 } };
    for (int c = 0; c < 4; c++)
      for (int k = 0; k < 4; k++) {
        const double hz = 440 * std::pow(2.0, (chords[c][k] - 69) / 12.0);
        for (size_t i = size_t(c * 2 * rate); i < size_t((c + 1) * 2 * rate); i++)
          for (int h = 1; h <= 3; h++) d[i] += float(.08 / h * std::sin(2 * 3.141592653589793 * hz * h * double(i) / rate));
      }
    const auto kb = forge::analyzeKeyBpm(d, rate);
    check(kb.key == 21, "Key: an Am - Dm - Em - Am progression reads as A minor", kb.key);
  }
  // ---- slicing / snapping
  {
    const auto ev = forge::evenSlices(8, 1.0, 3.0);
    check(ev.size() == 8 && std::abs(ev[3].start - 1.75) < 1e-9 && std::abs(ev[7].end - 3.0) < 1e-9, "Even slices: 8 equal cues", ev.size());
    std::vector<float> s(rate);
    for (size_t i = 0; i < s.size(); i++) s[i] = std::sin(2 * 3.14159265f * 100.f * float(i) / rate);
    const double z = forge::snapToZero(s, rate, .0123);
    check(std::abs(z - .010) < .0002 || std::abs(z - .015) < .0002, "Snap to zero crossing (100 Hz sine: 10 or 15 ms)", z * 1000);
    check(std::abs(forge::snapToBeat(.33, 120) - .375) < 1e-9, "Snap to beat: 0.33 s at 120 BPM -> 0.375 s (1/16 grid)", forge::snapToBeat(.33, 120));
  }

  std::printf(failures == 0 ? "\nALL NATIVE DSP TESTS PASSED\n" : "\n%d NATIVE DSP TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
