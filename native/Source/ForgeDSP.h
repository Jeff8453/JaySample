#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
#include <cstdint>
#include "signalsmith-stretch.h"
namespace forge {
struct Cue {
  double start = 0, end = 0;
  float pitch = 0, gain = .8f, cutoff = 20000, attack = .005f, release = .04f;
  bool reverse = false;
  int mode = 0;          // 0 trigger, 1 hold, 2 loop
  bool present = false;
  bool choke = false;    // retriggering this cue cuts its own previous hit
  std::string name;      // shown on the pad
};
struct Clip {
  std::array<std::vector<float>, 2> data;
  double sampleRate = 44100;
  int mode = 0;
  float release = .04f;
  double srcStart = 0, srcEnd = 0;   // where it came from in the sample (seconds), for the playhead
  bool reverse = false;
  bool choke = false;
  size_t size() const { return data[0].size(); }
};
inline std::shared_ptr<const Clip> render(const std::array<std::vector<float>, 2> &source,
                                          double sampleRate, const Cue &cue, double tempo,
                                          float transpose, bool formants) {
  if (!cue.present)
    return {};
  const size_t start = std::min(source[0].size(), size_t(std::max(0., cue.start) * sampleRate)),
               end = std::min(source[0].size(), size_t(std::max(0., cue.end) * sampleRate));
  if (end <= start || !std::isfinite(tempo) || tempo < .1 || tempo > 10)
    throw std::runtime_error("Invalid cue or tempo");
  const int frames = int(end - start), outFrames = std::max(1, int(std::round(frames / tempo)));
  if (outFrames > sampleRate * 600)
    throw std::runtime_error("Rendered cue exceeds ten minutes");
  auto out = std::make_shared<Clip>();
  out->sampleRate = sampleRate;
  out->mode = cue.mode;
  out->release = cue.release;
  out->srcStart = cue.start;
  out->srcEnd = cue.end;
  out->reverse = cue.reverse;
  out->choke = cue.choke;
  std::array<std::vector<float>, 2> input;
  for (int ch = 0; ch < 2; ch++) {
    input[ch].assign(source[ch].begin() + start, source[ch].begin() + end);
    if (cue.reverse)
      std::reverse(input[ch].begin(), input[ch].end());
    out->data[ch].resize(outFrames);
  }
  const float pitch = std::max(-36.f, std::min(36.f, cue.pitch + transpose));
  if (std::abs(pitch) < .0001f && std::abs(tempo - 1) < .000001) {
    out->data = input;
  } else {
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(2, sampleRate);
    stretch.setTransposeSemitones(pitch, std::min(8000., sampleRate / 2) / sampleRate);
    stretch.setFormantFactor(1, formants);
    const int padded = std::max(
                  frames,
                  int(std::ceil(stretch.inputLatency() + tempo * stretch.outputLatency())) + 2),
              paddedOut = std::max(outFrames, int(std::ceil(padded / tempo)));
    std::array<std::vector<float>, 2> processed;
    for (int ch = 0; ch < 2; ch++) {
      input[ch].resize(padded, 0);
      processed[ch].resize(paddedOut);
    }
    if (!stretch.exact(input, padded, processed, paddedOut))
      throw std::runtime_error("Spectral rendering failed");
    for (int ch = 0; ch < 2; ch++)
      std::copy_n(processed[ch].begin(), outFrames, out->data[ch].begin());
  }
  const double coefficient =
      1 - std::exp(-2 * 3.141592653589793 * std::min(double(cue.cutoff), sampleRate * .49) /
                   sampleRate);
  const int attack = std::min(outFrames / 2, int(cue.attack * sampleRate)),
            release = std::min(outFrames / 2, int(cue.release * sampleRate));
  // Attack is baked once; trigger release is applied by the voice, so loops
  // have no periodic release dip.
  (void)release;
  for (int ch = 0; ch < 2; ch++) {
    float filtered = 0;
    for (int i = 0; i < outFrames; i++) {
      filtered += float(coefficient) * (out->data[ch][i] - filtered);
      const float env = attack ? std::min(1.f, float(i) / attack) : 1.f;
      const int fade = std::min(outFrames / 4, int(sampleRate * .002));
      const float loopFade = cue.mode == 2 && fade > 0
                                 ? std::min(1.f, float(std::min(i, outFrames - 1 - i)) / fade)
                                 : 1.f;
      out->data[ch][i] = filtered * env * cue.gain * .8f * loopFade;
    }
  }
  return out;
}
struct Voice {
  const Clip *clip = nullptr;
  double position = 0, step = 1;
  float velocity = 1, releaseGain = 1;
  int note = -1, channel = 1;
  int pad = -1, cue = -1;    // which pad / cue started it (lights, choke, playhead)
  uint64_t order = 0;        // start order: the oldest is stolen first
  uint64_t kit = 0;          // generation of the prepared kit its clip belongs to
  bool releasing = false;
  float fadeIn = 1, fadeInStep = 0;     // 2 ms declick at the start
  bool killing = false;                 // fast fade-out: stolen, choked, bank swap, Stop
  float killGain = 1, killStep = 0;
  void start(const Clip *c, double hostRate, int n, int ch, float vel) {
    clip = c;
    position = 0;
    step = c->sampleRate / hostRate;
    velocity = vel;
    releaseGain = 1;
    note = n;
    channel = ch;
    releasing = false;
    killing = false;
    killGain = 1;
    fadeIn = 0;
    fadeInStep = float(1.0 / std::max(1.0, .002 * hostRate));
  }
  void release() { releasing = true; }
  // fade out over ms and stop (no click)
  void kill(double hostRate, double ms = 8) {
    if (!clip || killing)
      return;
    killing = true;
    killStep = float(1.0 / std::max(1.0, ms * .001 * hostRate));
  }
  bool active() const { return clip != nullptr; }
  bool sounding() const { return clip != nullptr && !killing; }
  // where in the clip it is (0..1), for the playhead
  double progress() const { return clip && clip->size() ? std::min(1.0, position / double(clip->size())) : 0.0; }
  void tick(float &left, float &right, double hostRate) {
    if (!clip)
      return;
    const auto length = clip->size();
    if (!length) {
      clip = nullptr;
      return;
    }
    if (position >= length) {
      if (clip->mode == 2 && !releasing)
        position = std::fmod(position, double(length));
      else {
        clip = nullptr;
        return;
      }
    }
    const size_t i = size_t(position), j = std::min(i + 1, length - 1);
    const float fraction = float(position - i);
    if (releasing) {
      releaseGain -= 1.f / std::max(1.f, clip->release * float(hostRate));
      if (releaseGain <= 0) {
        clip = nullptr;
        return;
      }
    }
    if (killing) {
      killGain -= killStep;
      if (killGain <= 0) {
        clip = nullptr;
        return;
      }
    }
    if (fadeIn < 1)
      fadeIn = std::min(1.f, fadeIn + fadeInStep);
    float envelope = releaseGain * velocity * fadeIn * (killing ? killGain : 1.f);
    if (clip->mode == 0) {
      const double remaining = (length - position) / step;
      envelope *= float(std::min(1., remaining / std::max(1., clip->release * hostRate)));
    }
    left += (clip->data[0][i] + fraction * (clip->data[0][j] - clip->data[0][i])) * envelope;
    right += (clip->data[1][i] + fraction * (clip->data[1][j] - clip->data[1][i])) * envelope;
    position += step;
  }
};

// Master bus: low cut + high cut (12 dB/oct), then a peak limiter that only acts above -1 dBFS.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void set(bool highpass, double hz, double rate) {
    const double w = 2 * 3.141592653589793 * std::min(hz, rate * .45) / rate, c = std::cos(w),
                 alpha = std::sin(w) / (2 * .7071), a0 = 1 + alpha;
    if (highpass) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; }
    else { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
  }
  float process(float x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return float(y);
  }
  void reset() { z1 = z2 = 0; }
};
struct MasterBus {
  Biquad hp[2], lp[2];
  double rate = 44100;
  float lowCut = 20, highCut = 20000, gain = 1, releaseCoef = .9999f;
  bool hpOn = false, lpOn = false;
  static constexpr float threshold = 0.891f;   // -1 dBFS
  void prepare(double r) {
    rate = r;
    for (auto &f : hp) f.reset();
    for (auto &f : lp) f.reset();
    gain = 1;
    releaseCoef = float(std::exp(-1.0 / (.12 * r)));   // 120 ms recovery
    setFilters(lowCut, highCut, true);
  }
  void setFilters(float low, float high, bool force = false) {
    if (!force && std::abs(low - lowCut) < .01f && std::abs(high - highCut) < .5f)
      return;
    lowCut = low; highCut = high;
    hpOn = low > 20.5f;
    lpOn = high < 19999.f;
    for (int c = 0; c < 2; c++) { hp[c].set(true, low, rate); lp[c].set(false, high, rate); }
  }
  void process(float &l, float &r) {
    if (hpOn) { l = hp[0].process(l); r = hp[1].process(r); }
    if (lpOn) { l = lp[0].process(l); r = lp[1].process(r); }
    const float peak = std::max(std::abs(l), std::abs(r));
    const float want = peak > threshold ? threshold / peak : 1.f;
    gain = want < gain ? want : 1.f - (1.f - gain) * releaseCoef;   // instant down, smooth back up
    l = std::max(-1.f, std::min(1.f, l * gain));
    r = std::max(-1.f, std::min(1.f, r * gain));
  }
};
} // namespace forge
