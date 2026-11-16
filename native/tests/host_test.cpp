// JaySample host test: drives the real plugin (processor + editor) without a DAW.
// Loads a WAV through the plugin, plays MIDI, checks quantize timing, saves / restores state, exports a cue,
// and writes a screenshot of the window. Build with -DJAYSAMPLE_HOST_TEST=ON.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what, double value) {
  std::printf("%s %-80s (%.4f)\n", ok ? "ok  " : "FAIL", what, value);
  if (!ok) failures++;
}

struct TestHead : juce::AudioPlayHead {
  double bpm = 120, ppq = 0;
  bool playing = false;
  juce::Optional<PositionInfo> getPosition() const override {
    PositionInfo p;
    p.setBpm(bpm);
    p.setPpqPosition(ppq);
    p.setIsPlaying(playing);
    return p;
  }
};

static void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

static bool waitFor(std::function<bool()> cond, int ms) {
  for (int t = 0; t < ms; t += 20) { if (cond()) return true; pump(20); }
  return cond();
}

static float block(ForgeProcessor &p, juce::MidiBuffer &midi, int n = 512, std::vector<float> *keep = nullptr) {
  juce::AudioBuffer<float> buf(2, n);
  p.processBlock(buf, midi);
  if (keep) for (int i = 0; i < n; i++) keep->push_back(buf.getSample(0, i));
  return buf.getMagnitude(0, n);
}

int main() {
  juce::ScopedJuceInitialiser_GUI gui;
  const double rate = 44100;
  // a 92 BPM loop: kick-like hits on every beat, louder on the 1, with a tone in A
  const juce::File wavFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sf_host_test.wav");
  {
    const int n = int(rate * 8);
    juce::AudioBuffer<float> b(2, n);
    b.clear();
    juce::Random rng(7);
    const double beat = 60.0 / 92;
    for (int k = 0; k * beat < 7.8; k++) {
      const int at = int(k * beat * rate);
      const float amp = k % 4 == 0 ? .8f : .45f;
      for (int i = 0; i < int(.2 * rate) && at + i < n; i++) {
        const float env = std::exp(-float(i) / float(.035 * rate));
        const float x = amp * env * (.5f * (rng.nextFloat() * 2 - 1) + .5f * std::sin(2 * 3.14159265f * 110.f * float(i) / float(rate)));
        b.addSample(0, at + i, x);
        b.addSample(1, at + i, x);
      }
    }
    wavFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(new juce::FileOutputStream(wavFile), rate, 2, 24, {}, 0));
    w->writeFromAudioSampleBuffer(b, 0, n);
  }

  auto proc = std::make_unique<ForgeProcessor>();
  TestHead head;
  proc->setPlayHead(&head);
  proc->prepareToPlay(rate, 512);
  proc->loadAudioFile(wavFile);
  const bool loaded = waitFor([&] { return !proc->isLoading() && proc->project() != nullptr; }, 20000);
  auto pr = proc->project();
  int cues = 0;
  if (pr) for (int i = 0; i < 16; i++) cues += pr->cues[size_t(i)].present ? 1 : 0;
  check(loaded && pr && std::abs(pr->sourceBpm - 92) < 1.5, "load a WAV in the plugin: BPM detected as 92", pr ? pr->sourceBpm : 0);
  check(cues == 12, "Find Samples on load: one pad per hit (12 hits) on bank A", cues);
  bool ordered = true;
  if (pr) for (int i = 1; i < cues; i++) ordered = ordered && pr->cues[size_t(i)].start > pr->cues[size_t(i - 1)].start;
  check(ordered && pr && std::abs(pr->cues[0].start) < .01, "pads in time order, pad 1 on the first hit", pr ? pr->cues[0].start : -1);

  // host tempo 92 = the sample's: no stretching needed; wait for Ready
  head.bpm = 92;
  { juce::MidiBuffer m; block(*proc, m); }
  const bool ready = waitFor([&] { return proc->status().startsWith("Ready"); }, 30000);
  check(ready, "prepared audio ready", 0);

  // play pad 1 (C1 = 36)
  {
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120), 0);
    float peak = 0;
    peak = std::max(peak, block(*proc, m));
    for (int i = 0; i < 8; i++) { juce::MidiBuffer e; peak = std::max(peak, block(*proc, e)); }
    check(peak > .05f && peak <= .8913f, "pad 1 plays from MIDI; output stays under -1 dBFS", peak);
    check((proc->padSounding.load() & 1) != 0 && proc->padHits[0].load() == 1, "pad 1 lights (sounding + hit count)", proc->padSounding.load());
    check(proc->playCue.load() == 0 && proc->playPos.load() > 0 && proc->playPos.load() < .3, "playhead follows pad 1", proc->playPos.load());
  }
  // Stop fades out (no click), then silence
  {
    proc->panic = true;
    std::vector<float> out;
    juce::MidiBuffer e;
    block(*proc, e, 512, &out);
    for (int i = 0; i < 4; i++) { juce::MidiBuffer e2; block(*proc, e2, 512, &out); }
    float tail = 0;
    for (size_t i = 1024; i < out.size(); i++) tail = std::max(tail, std::abs(out[i]));
    check(tail < 1e-4f, "STOP fades everything out within ~10 ms", tail);
  }
  // quantize 1/4: a hit at beat 0.1 waits for beat 1
  {
    proc->parameters.getParameter("quantize")->setValueNotifyingHost(proc->parameters.getParameter("quantize")->convertTo0to1(1));
    head.playing = true;
    head.bpm = 92;
    head.ppq = .1;
    std::vector<float> out;
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120), 0);
    block(*proc, m, 512, &out);
    for (int i = 0; i < 70; i++) { juce::MidiBuffer e; head.ppq += 512 / rate * 92 / 60; block(*proc, e, 512, &out); }
    size_t first = 0;
    while (first < out.size() && std::abs(out[first]) < 1e-6f) first++;
    const double want = .9 * 60 / 92 * rate;
    check(std::abs(double(first) - want) < 64, "Quantize 1/4: a hit at beat 0.1 lands on beat 1", double(first) - want);
    head.playing = false;
    proc->parameters.getParameter("quantize")->setValueNotifyingHost(0);
    proc->panic = true;
    juce::MidiBuffer e; block(*proc, e);
  }
  // choke / mono: a re-hit of a choke pad cuts its last hit (fading)
  {
    proc->editCue(1, [](forge::Cue &q) { q.choke = true; });
    pump(300);
    waitFor([&] { return proc->status().startsWith("Ready"); }, 20000);
    { juce::MidiBuffer e; block(*proc, e); }
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 37, (juce::uint8) 100), 0);
    m.addEvent(juce::MidiMessage::noteOn(1, 37, (juce::uint8) 100), 300);
    block(*proc, m);
    for (int i = 0; i < 4; i++) { juce::MidiBuffer e; block(*proc, e); }
    check(proc->activeVoices.load() == 1, "Choke: the second hit of pad 2 cut the first", proc->activeVoices.load());
    proc->panic = true;
    juce::MidiBuffer e; block(*proc, e);
  }
  // key shift re-renders and still plays
  {
    proc->parameters.getParameter("transpose")->setValueNotifyingHost(proc->parameters.getParameter("transpose")->convertTo0to1(3));
    pump(300);
    const bool r2 = waitFor([&] { return proc->status().startsWith("Ready"); }, 30000);
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 120), 0);
    float peak = block(*proc, m);
    for (int i = 0; i < 6; i++) { juce::MidiBuffer e; peak = std::max(peak, block(*proc, e)); }
    check(r2 && peak > .05f, "Key Shift +3: re-renders and plays", peak);
    proc->panic = true;
    juce::MidiBuffer e; block(*proc, e);
  }
  // PAD KEYS: move to C2, then 48 plays pad 1
  {
    proc->parameters.getParameter("padbase")->setValueNotifyingHost(proc->parameters.getParameter("padbase")->convertTo0to1(48));
    const int before = proc->padHits[0].load();
    juce::MidiBuffer m;
    m.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 100), 0);
    block(*proc, m);
    check(proc->padHits[0].load() == before + 1, "PAD KEYS at C2: note 48 plays pad 1", proc->padHits[0].load() - before);
    proc->parameters.getParameter("padbase")->setValueNotifyingHost(proc->parameters.getParameter("padbase")->convertTo0to1(36));
    proc->panic = true;
    juce::MidiBuffer e; block(*proc, e);
  }
  // save into the Live set and restore into a fresh instance
  {
    juce::MemoryBlock state;
    proc->getStateInformation(state);
    auto fresh = std::make_unique<ForgeProcessor>();
    fresh->setStateInformation(state.getData(), int(state.getSize()));
    auto a = proc->project(), b = fresh->project();
    bool same = a && b && std::abs(a->sourceBpm - b->sourceBpm) < 1e-9 && a->sample->audio[0].size() == b->sample->audio[0].size();
    float err = 0;
    if (same) {
      for (int i = 0; i < 64; i++)
        same = same && a->cues[size_t(i)].present == b->cues[size_t(i)].present && std::abs(a->cues[size_t(i)].start - b->cues[size_t(i)].start) < 1e-9 &&
               a->cues[size_t(i)].choke == b->cues[size_t(i)].choke && a->cues[size_t(i)].name == b->cues[size_t(i)].name;
      for (size_t i = 0; i < a->sample->audio[0].size(); i++) err = std::max(err, std::abs(a->sample->audio[0][i] - b->sample->audio[0][i]));
    }
    const bool params = std::abs(fresh->parameters.getRawParameterValue("transpose")->load() - 3) < .01f;
    check(same && err < 1e-5f && params, "saved in the Live set (FLAC) and restored: audio, pads, choke, names, Key Shift", err);
    std::printf("     state size for an 8 s sample: %.1f KB\n", state.getSize() / 1024.0);
  }
  // export a cue as WAV (drag to Live)
  {
    juce::String msg;
    const auto f = proc->exportCue(0, msg);
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(f.existsAsFile() ? fm.createReaderFor(f) : nullptr);
    check(r != nullptr && r->lengthInSamples > 1000, "drag-out: pad 1 exports as a WAV", r ? double(r->lengthInSamples) : 0);
    if (f.existsAsFile()) f.deleteFile();
  }
  // the window: build it and save a picture
  {
    std::unique_ptr<juce::AudioProcessorEditor> ed(proc->createEditor());
    pump(200);
    {
      juce::MidiBuffer m;
      m.addEvent(juce::MidiMessage::noteOn(1, 38, (juce::uint8) 120), 0);
      block(*proc, m);
      for (int i = 0; i < 6; i++) { juce::MidiBuffer e; block(*proc, e); }
    }
    pump(150);
    auto *view = ed->getChildComponent(0);
    const auto img = view->createComponentSnapshot(view->getLocalBounds(), true, 1.0f);
    juce::File png("/tmp/jaysample_ui.png");
    png.deleteFile();
    juce::FileOutputStream os(png);
    juce::PNGImageFormat().writeImageToStream(img, os);
    check(ed->getWidth() > 500 && png.getSize() > 10000, "editor opens; screenshot written", ed->getWidth());
    ed.reset();
  }
  proc->releaseResources();
  proc.reset();
  wavFile.deleteFile();
  std::printf(failures == 0 ? "\nALL HOST TESTS PASSED\n" : "\n%d HOST TEST(S) FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
