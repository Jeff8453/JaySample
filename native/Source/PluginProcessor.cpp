#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
void set(juce::AudioProcessorValueTreeState &p, const char *id, float v) {
  if (auto *param = p.getParameter(id)) {
    param->beginChangeGesture();
    param->setValueNotifyingHost(param->convertTo0to1(v));
    param->endChangeGesture();
  }
}
juce::String cueKey(const forge::Cue &q) {
  return juce::String(q.start, 6) + "|" + juce::String(q.end, 6) + "|" + juce::String(q.pitch, 3) + "|" + juce::String(q.gain, 4) + "|" +
         juce::String(q.cutoff, 1) + "|" + juce::String(q.attack, 4) + "|" + juce::String(q.release, 4) + "|" +
         juce::String(int(q.reverse)) + juce::String(q.mode) + juce::String(int(q.choke));
}
} // namespace

juce::AudioProcessorValueTreeState::ParameterLayout ForgeProcessor::layout() {
  using namespace juce;
  std::vector<std::unique_ptr<RangedAudioParameter>> p;
  auto hz = [](float v, int) { return v >= 1000.f ? String(v / 1000.f, 1) + " kHz" : String(roundToInt(v)) + " Hz"; };
  p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"master", 1}, "Master", NormalisableRange<float>(0.f, 1.f), .8f,
                                                    AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) { return String(roundToInt(v * 100)) + " %"; })));
  p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"bpm", 1}, "Target BPM", NormalisableRange<float>(30.f, 300.f, .1f), 120.f,
                                                    AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) { return String(v, 1) + " BPM"; })));
  p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"transpose", 1}, "Key Shift", NormalisableRange<float>(-12.f, 12.f, 1.f), 0.f,
                                                    AudioParameterFloatAttributes().withStringFromValueFunction([](float v, int) { const int s = roundToInt(v); return (s > 0 ? "+" : "") + String(s) + " st"; })));
  p.push_back(std::make_unique<AudioParameterInt>(ParameterID{"bank", 1}, "Bank", 0, 3, 0,
                                                  AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return String::charToString(juce_wchar('A' + v)); })));
  p.push_back(std::make_unique<AudioParameterInt>(ParameterID{"cue", 1}, "Selected Pad", 0, 15, 0));
  p.push_back(std::make_unique<AudioParameterBool>(ParameterID{"sync", 1}, "Sync to Host Tempo", true));
  p.push_back(std::make_unique<AudioParameterBool>(ParameterID{"keyboard", 1}, "Keyboard Mode", false));
  p.push_back(std::make_unique<AudioParameterBool>(ParameterID{"poly", 1}, "Polyphonic", true));
  p.push_back(std::make_unique<AudioParameterBool>(ParameterID{"formants", 1}, "Preserve Formants", false));
  p.push_back(std::make_unique<AudioParameterChoice>(ParameterID{"quantize", 1}, "Quantize", StringArray{"Off", "1/4", "1/8", "1/16"}, 0));
  p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"lowcut", 1}, "Low Cut", NormalisableRange<float>(20.f, 1000.f, 0.f, .35f), 20.f,
                                                    AudioParameterFloatAttributes().withStringFromValueFunction([hz](float v, int n) { return v < 20.5f ? String("Off") : hz(v, n); })));
  p.push_back(std::make_unique<AudioParameterFloat>(ParameterID{"highcut", 1}, "High Cut", NormalisableRange<float>(500.f, 20000.f, 0.f, .35f), 20000.f,
                                                    AudioParameterFloatAttributes().withStringFromValueFunction([hz](float v, int n) { return v > 19999.f ? String("Off") : hz(v, n); })));
  p.push_back(std::make_unique<AudioParameterInt>(ParameterID{"padbase", 1}, "Pad Keys", 0, 112, 36,
                                                  AudioParameterIntAttributes().withStringFromValueFunction([](int v, int) { return MidiMessage::getMidiNoteName(v, true, true, 3); })));
  return {p.begin(), p.end()};
}

ForgeProcessor::ForgeProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("JaySample renderer"), parameters(*this, nullptr, "JaySample", layout()) {
  pMaster = parameters.getRawParameterValue("master");     pBpm = parameters.getRawParameterValue("bpm");
  pTranspose = parameters.getRawParameterValue("transpose"); pBank = parameters.getRawParameterValue("bank");
  pCue = parameters.getRawParameterValue("cue");           pSync = parameters.getRawParameterValue("sync");
  pKeyboard = parameters.getRawParameterValue("keyboard"); pPoly = parameters.getRawParameterValue("poly");
  pFormants = parameters.getRawParameterValue("formants"); pQuantize = parameters.getRawParameterValue("quantize");
  pLowCut = parameters.getRawParameterValue("lowcut");     pHighCut = parameters.getRawParameterValue("highcut");
  pPadBase = parameters.getRawParameterValue("padbase");
  startThread();
  startTimerHz(10);
}

ForgeProcessor::~ForgeProcessor() {
  stopTimer();
  alive.reset();
  loader.removeAllJobs(true, 20000);
  signalThreadShouldExit();
  wake.signal();
  stopThread(-1);
}

void ForgeProcessor::prepareToPlay(double rate, int) {
  hostRate = rate;
  volume.reset(rate, .015);
  volume.setCurrentAndTargetValue(pMaster->load());
  bus.prepare(rate);
  for (auto &v : voices) v.clip = nullptr;
}

void ForgeProcessor::releaseResources() {
  for (auto &v : voices) v.clip = nullptr;
}

bool ForgeProcessor::isBusesLayoutSupported(const BusesLayout &b) const {
  return b.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

std::shared_ptr<const ForgeProject> ForgeProcessor::project() const {
  const juce::ScopedLock lock(stateLock);
  return source;
}

juce::String ForgeProcessor::status() const {
  const juce::ScopedLock lock(stateLock);
  return statusText;
}

void ForgeProcessor::setProject(std::shared_ptr<const ForgeProject> p, const juce::String &message) {
  const juce::ScopedLock lock(stateLock);
  source = std::move(p);
  if (message.isNotEmpty()) statusText = message;
}

// ---------------------------------------------------------------- loading

std::shared_ptr<SampleData> ForgeProcessor::analyse(std::array<std::vector<float>, 2> &&audio, double rate, const juce::String &name,
                                                    const juce::String &path) {
  auto s = std::make_shared<SampleData>();
  s->audio = std::move(audio);
  s->rate = rate;
  s->name = name;
  s->path = path;
  s->mono = forge::mixToMono(s->audio);
  s->peaks = forge::makePeaks(s->mono);
  s->onsets = forge::detectOnsets(s->mono, rate);
  return s;
}

void ForgeProcessor::autoCues(ForgeProject &p, int bank, float sensitivity, uint32_t seed) {
  const auto &s = *p.sample;
  auto slices = forge::findSamples(s.onsets, s.length(), 16, sensitivity, seed);
  if (slices.empty()) slices = forge::evenSlices(16, 0, s.length());   // no hits (a pad, a drone): even slices
  for (int i = 0; i < 16; i++) {
    auto &q = p.cues[size_t(bank * 16 + i)];
    const bool keepSettings = q.present;
    forge::Cue fresh;
    if (keepSettings) fresh = q;   // keep mode / pitch / gain etc. on a re-roll
    if (i < int(slices.size())) {
      fresh.present = true;
      fresh.start = slices[size_t(i)].start;
      fresh.end = slices[size_t(i)].end;
      if (fresh.name.empty()) fresh.name = "Sample " + std::to_string(i + 1);
    } else fresh = forge::Cue();
    q = fresh;
  }
}

void ForgeProcessor::loadAudioFile(const juce::File &file) {
  if (!file.existsAsFile()) return;
  loading = true;
  {
    const juce::ScopedLock lock(stateLock);
    statusText = "Loading " + file.getFileName() + " ...";
  }
  std::weak_ptr<int> life = alive;
  const float sens = sensitivity;
  loader.addJob([this, file, life, sens] {
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    juce::String error;
    std::shared_ptr<ForgeProject> p;
    double bpm = 0;
    if (reader == nullptr) error = "Can't read " + file.getFileName();
    else if (reader->lengthInSamples > juce::int64(reader->sampleRate * 600)) error = "Samples up to 10 minutes";
    else {
      const int n = int(reader->lengthInSamples);
      juce::AudioBuffer<float> buf(2, n);
      buf.clear();
      reader->read(&buf, 0, n, 0, true, true);
      if (reader->numChannels < 2) buf.copyFrom(1, 0, buf, 0, 0, n);
      std::array<std::vector<float>, 2> audio;
      for (int c = 0; c < 2; c++) {
        audio[size_t(c)].assign(buf.getReadPointer(c), buf.getReadPointer(c) + n);
        for (auto &x : audio[size_t(c)]) if (!std::isfinite(x)) x = 0;
      }
      p = std::make_shared<ForgeProject>();
      p->sample = analyse(std::move(audio), reader->sampleRate, file.getFileNameWithoutExtension(), file.getFullPathName());
      const auto kb = forge::analyzeKeyBpm(p->sample->mono, reader->sampleRate);
      bpm = kb.bpm;
      p->sourceBpm = kb.bpm > 0 ? kb.bpm : 120;
      p->bpmDetected = kb.bpm > 0;
      p->sourceKey = kb.key;
      p->keyDetected = kb.key >= 0;
      autoCues(*p, 0, sens, 0);   // like loading into Serato Sample: bank A gets the 16 best hits
    }
    juce::MessageManager::callAsync([this, life, p, error, bpm] {
      if (life.expired()) return;
      loading = false;
      if (p == nullptr) {
        const juce::ScopedLock lock(stateLock);
        statusText = error;
        return;
      }
      auto q = std::make_shared<ForgeProject>(*p);
      {
        const juce::ScopedLock lock(stateLock);
        q->version = ++versionCounter;
      }
      setProject(q, "Loaded " + q->sample->name + "  |  " + (q->bpmDetected ? juce::String(bpm, 1) + " BPM" : juce::String("BPM ?")) + "  |  " +
                        (q->keyDetected ? juce::String(forge::keyName(q->sourceKey)) : juce::String("key ?")));
      set(parameters, "bank", 0);
      set(parameters, "cue", 0);
      set(parameters, "transpose", 0);
      panic = true;
    });
  });
}

// the browser's .sforge projects (version 2): sample + 64 cues
juce::Result ForgeProcessor::loadSforge(const juce::MemoryBlock &bytes, bool defaults) {
  try {
    if (bytes.getSize() < 12 || bytes.getSize() > 512u * 1024u * 1024u) throw std::runtime_error("Invalid project size");
    const auto *d = static_cast<const uint8_t *>(bytes.getData());
    if (std::memcmp(d, "SFORGE02", 8)) throw std::runtime_error("Expected a version 2 .sforge project");
    const uint32_t size = juce::ByteOrder::littleEndianInt(d + 8);
    if (size > 1024u * 1024u || size + 12u > bytes.getSize()) throw std::runtime_error("Invalid metadata");
    auto root = juce::JSON::parse(juce::String::fromUTF8(reinterpret_cast<const char *>(d + 12), int(size)));
    if (!root.isObject()) throw std::runtime_error("Invalid project JSON");
    const auto a = root["audio"];
    const int frames = int(a["frames"]), channels = int(a["channels"]), rate = int(a["sampleRate"]);
    if (int(root["version"]) != 2 || frames < 1 || channels < 1 || channels > 2 || rate < 8000 || rate > 192000 ||
        double(frames) / rate > 600 || bytes.getSize() != 12u + size + size_t(frames) * size_t(channels) * 4)
      throw std::runtime_error("Invalid audio dimensions");
    std::array<std::vector<float>, 2> audio;
    for (int ch = 0; ch < 2; ch++) {
      audio[size_t(ch)].resize(size_t(frames));
      std::memcpy(audio[size_t(ch)].data(), d + 12 + size + size_t(std::min(ch, channels - 1)) * size_t(frames) * 4, size_t(frames) * 4);
      for (float f : audio[size_t(ch)])
        if (!std::isfinite(f) || std::abs(f) > 32) throw std::runtime_error("Invalid PCM data");
    }
    auto p = std::make_shared<ForgeProject>();
    p->sample = analyse(std::move(audio), rate, root["name"].toString(), {});
    const auto c = root["controls"];
    p->sourceBpm = double(c["sourcebpm"]);
    if (!std::isfinite(p->sourceBpm) || p->sourceBpm < 30 || p->sourceBpm > 300) throw std::runtime_error("Invalid source BPM");
    p->bpmDetected = true;
    float shift = 0;
    if (c["sourcekey"].toString().isNotEmpty()) {
      const int from = int(c["sourcekey"]);
      if (from < 0 || from > 23) throw std::runtime_error("Invalid key");
      p->sourceKey = from;
      p->keyDetected = true;
      if (c["targetkey"].toString().isNotEmpty()) {
        const int to = int(c["targetkey"]);
        if (to < 0 || to > 23) throw std::runtime_error("Invalid key");
        shift = float(((to % 12 - from % 12 + 18) % 12) - 6);
      }
    }
    const auto *banks = root["banks"].getArray();
    if (!banks || banks->size() != 4) throw std::runtime_error("Invalid banks");
    for (int b = 0; b < 4; b++) {
      const auto *cues = banks->getReference(b).getArray();
      if (!cues || cues->size() != 16) throw std::runtime_error("Invalid cue count");
      for (int i = 0; i < 16; i++) {
        const auto &v = cues->getReference(i);
        if (v.isVoid()) continue;
        auto &q = p->cues[size_t(b * 16 + i)];
        q.present = true;
        q.start = double(v["start"]);
        q.end = double(v["end"]);
        q.pitch = float(v["pitch"]);
        q.gain = float(v["gain"]) / 100;
        q.cutoff = float(v["filter"]);
        q.attack = float(v["attack"]) / 1000;
        q.release = float(v["release"]) / 1000;
        q.reverse = bool(v["reverse"]);
        q.name = v["name"].toString().toStdString();
        const auto mode = v["mode"].toString();
        q.mode = mode == "loop" ? 2 : mode == "hold" ? 1 : 0;
        if (!std::isfinite(q.start) || !std::isfinite(q.end) || q.start < 0 || q.end > double(frames) / rate || q.end - q.start < 1. / rate ||
            !std::isfinite(q.pitch) || std::abs(q.pitch) > 24 || !std::isfinite(q.gain) || q.gain < 0 || q.gain > 1 ||
            !std::isfinite(q.cutoff) || q.cutoff < 100 || q.cutoff > 20000 || !std::isfinite(q.attack) || q.attack < 0 || q.attack > 2 ||
            !std::isfinite(q.release) || q.release < 0 || q.release > 5 || !(mode == "loop" || mode == "hold" || mode == "trigger"))
          throw std::runtime_error("Invalid cue parameters");
      }
    }
    {
      const juce::ScopedLock lock(stateLock);
      p->version = ++versionCounter;
    }
    setProject(p, "Opened " + p->sample->name);
    if (defaults) {
      set(parameters, "bpm", float(c["targetbpm"]));
      set(parameters, "transpose", juce::jlimit(-12.f, 12.f, shift));
      set(parameters, "master", c.hasProperty("master") ? float(c["master"]) / 100 : .8f);
      set(parameters, "formants", bool(c["formants"]) ? 1 : 0);
      set(parameters, "poly", c["voices"].toString() == "mono" ? 0 : 1);
    }
    panic = true;
    return juce::Result::ok();
  } catch (const std::exception &e) {
    return juce::Result::fail(e.what());
  }
}

// ---------------------------------------------------------------- edits (message thread)

void ForgeProcessor::editProject(const std::function<void(ForgeProject &)> &fn) {
  auto cur = project();
  if (!cur) return;
  auto p = std::make_shared<ForgeProject>(*cur);
  fn(*p);
  {
    const juce::ScopedLock lock(stateLock);
    p->version = ++versionCounter;
    source = p;
  }
}

void ForgeProcessor::editCue(int index, const std::function<void(forge::Cue &)> &fn) {
  if (index < 0 || index >= 64) return;
  editProject([&](ForgeProject &p) {
    auto &q = p.cues[size_t(index)];
    fn(q);
    const double len = p.sample ? p.sample->length() : 0;
    q.start = juce::jlimit(0.0, std::max(0.0, len - .005), q.start);
    q.end = juce::jlimit(q.start + .005, std::max(q.start + .005, len), q.end);
  });
}

void ForgeProcessor::findSamples(int bank, float sens, bool reroll) {
  sensitivity = sens;
  auto cur = project();
  if (!cur || !cur->sample) return;
  uint32_t seed = 0;
  if (reroll) {
    // try seeds until the set is different from what's there now
    seed = juce::Random::getSystemRandom().nextInt(1 << 30) + 1;
    for (int tries = 0; tries < 12; tries++, seed++) {
      ForgeProject probe = *cur;
      autoCues(probe, bank, sens, seed);
      bool same = true;
      for (int i = 0; i < 16; i++)
        if (std::abs(probe.cues[size_t(bank * 16 + i)].start - cur->cues[size_t(bank * 16 + i)].start) > .005) same = false;
      if (!same) break;
    }
  }
  editProject([&](ForgeProject &p) { autoCues(p, bank, sens, seed); });
}

void ForgeProcessor::sliceEvenly(int bank, int count) {
  editProject([&](ForgeProject &p) {
    const double len = p.sample->length();
    // a whole number of bars when the tempo is known, otherwise the whole sample
    double to = len;
    if (p.sourceBpm > 0) {
      const double bar = 240.0 / p.sourceBpm;
      if (len > bar) to = std::floor(len / bar) * bar;
    }
    const auto s = forge::evenSlices(juce::jlimit(1, 16, count), 0, to);
    for (int i = 0; i < 16; i++) {
      auto &q = p.cues[size_t(bank * 16 + i)];
      if (i < int(s.size())) {
        const bool had = q.present;
        if (!had) q = forge::Cue();
        q.present = true;
        q.start = s[size_t(i)].start;
        q.end = s[size_t(i)].end;
        if (q.name.empty()) q.name = "Slice " + std::to_string(i + 1);
      } else q = forge::Cue();
    }
  });
}

void ForgeProcessor::addCueAt(int index, double t) {
  editCue(index, [&](forge::Cue &q) {
    auto p = project();
    double end = t + 1.0;
    if (p && p->sample)
      for (const auto &o : p->sample->onsets) if (o.time > t + .08) { end = std::min(end, o.time); break; }
    if (!q.present) { q = forge::Cue(); q.name = "Sample " + std::to_string(index % 16 + 1); }
    q.present = true;
    q.start = t;
    q.end = end;
  });
}

// rendered at the current key / tempo, 24-bit WAV in ~/Music/JaySample Exports
juce::File ForgeProcessor::exportCue(int index, juce::String &message) {
  auto p = project();
  if (!p || !p->sample || index < 0 || index >= 64 || !p->cues[size_t(index)].present) { message = "Empty pad"; return {}; }
  const double target = pSync->load() > .5f ? hostBpm.load() : double(pBpm->load());
  try {
    auto clip = forge::render(p->sample->audio, p->sample->rate, p->cues[size_t(index)], juce::jlimit(30.0, 300.0, target) / p->sourceBpm,
                              pTranspose->load(), pFormants->load() > .5f);
    if (!clip) { message = "Empty pad"; return {}; }
    auto dir = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("JaySample Exports");
    dir.createDirectory();
    const auto &q = p->cues[size_t(index)];
    const juce::String bank = juce::String::charToString(juce_wchar('A' + index / 16));
    auto file = dir.getChildFile(juce::File::createLegalFileName(p->sample->name + " " + bank + juce::String(index % 16 + 1) + " " +
                                                                  juce::String(q.name) + ".wav")).getNonexistentSibling();
    std::unique_ptr<juce::FileOutputStream> os(file.createOutputStream());
    if (!os) { message = "Can't write file"; return {}; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w(wav.createWriterFor(os.get(), clip->sampleRate, 2, 24, {}, 0));
    if (!w) { message = "Can't write WAV"; return {}; }
    os.release();
    const float *chans[2] = {clip->data[0].data(), clip->data[1].data()};
    w->writeFromFloatArrays(chans, 2, int(clip->size()));
    w.reset();
    message = "Exported " + file.getFileName();
    return file;
  } catch (const std::exception &e) {
    message = e.what();
    return {};
  }
}

// ---------------------------------------------------------------- rendering

void ForgeProcessor::timerCallback() {
  if (const int ln = padLearned.exchange(-1); ln >= 0) set(parameters, "padbase", float(juce::jlimit(0, 112, ln)));
  const auto p = project();
  if (!p || !p->sample) return;
  const double target = pSync->load() > .5f ? hostBpm.load() : double(pBpm->load());
  Job job;
  job.project = p;
  job.target = juce::jlimit(30., 300., target);
  job.tempo = job.target / std::max(1.0, p->sourceBpm);
  job.transpose = pTranspose->load();
  job.formants = pFormants->load() > .5f;
  job.keyboard = pKeyboard->load() > .5f;
  job.bank = int(pBank->load());
  job.selected = int(pCue->load());
  const juce::String signature = juce::String((juce::int64) p->version) + ":" + juce::String(job.target, 1) + ":" + juce::String(job.transpose, 2) + ":" +
                                 juce::String(job.formants ? 1 : 0) + ":" + juce::String(job.keyboard ? 1 : 0) + ":" +
                                 (job.keyboard ? juce::String(job.bank * 16 + job.selected) : juce::String());
  {
    const juce::ScopedLock lock(stateLock);
    if (signature == lastSignature) return;
    lastSignature = signature;
    job.generation = ++nextGeneration;
    requested = job;
    if (!loading) statusText = "Preparing ...";
  }
  wake.signal();
}

void ForgeProcessor::run() {
  while (!threadShouldExit()) {
    wake.wait(100);
    if (threadShouldExit()) break;
    Job job;
    {
      const juce::ScopedLock lock(stateLock);
      job = requested;
      requested.project.reset();
    }
    if (!job.project || !job.project->sample) continue;
    try {
      auto kit = std::make_shared<ForgeKit>();
      kit->generation = job.generation;
      kit->bank = job.bank;
      kit->selected = job.selected;
      kit->keyboard = job.keyboard;
      kit->targetBpm = job.target;
      size_t bytes = 0;
      bool stale = false;
      std::map<std::string, std::shared_ptr<const forge::Clip>> used;
      const std::string common = juce::String(job.tempo, 6).toStdString() + "|" + juce::String(job.formants ? 1 : 0).toStdString() + "|" +
                                 std::to_string(reinterpret_cast<uintptr_t>(job.project->sample.get()));
      auto add = [&](const forge::Cue &q, float extra) -> std::shared_ptr<const forge::Clip> {
        if (!q.present) return {};
        const std::string key = cueKey(q).toStdString() + "|" + common + "|" + juce::String(job.transpose + extra, 3).toStdString();
        std::shared_ptr<const forge::Clip> clip;
        if (auto it = clipCache.find(key); it != clipCache.end()) clip = it->second;   // unchanged: reuse
        else clip = forge::render(job.project->sample->audio, job.project->sample->rate, q, job.tempo, job.transpose + extra, job.formants);
        if (clip) { used[key] = clip; bytes += clip->size() * 8; }
        if (bytes > 384u * 1024u * 1024u) throw std::runtime_error("Prepared audio over 384 MB: use shorter cues");
        return clip;
      };
      auto isStale = [&] {
        const juce::ScopedLock lock(stateLock);
        return job.generation != nextGeneration;
      };
      for (int i = 0; i < 64 && !stale; i++) {
        if (threadShouldExit() || isStale()) { stale = true; break; }
        kit->cues[size_t(i)] = add(job.project->cues[size_t(i)], 0);
      }
      if (job.keyboard)
        for (int n = 0; n < 49 && !stale; n++) {
          if (threadShouldExit() || isStale()) { stale = true; break; }
          kit->keys[size_t(n)] = add(job.project->cues[size_t(job.bank * 16 + job.selected)], float(n - 24));
        }
      if (stale) continue;
      clipCache.swap(used);
      {
        const juce::ScopedLock lock(stateLock);
        if (job.generation != nextGeneration) continue;
        std::shared_ptr<const ForgeKit> ready = kit;
        retained.push_back(ready);
        std::atomic_store(&published, ready);
        if (!loading) statusText = "Ready  |  " + juce::String(job.target, 1) + " BPM  |  " + juce::String(double(bytes) / 1048576.0, 1) + " MB";
      }
      retained.erase(std::remove_if(retained.begin(), retained.end(), [](const auto &k) { return k.use_count() == 1; }), retained.end());
    } catch (const std::exception &e) {
      const juce::ScopedLock lock(stateLock);
      statusText = juce::String("Can't prepare: ") + e.what();
    }
  }
}

// ---------------------------------------------------------------- audio thread

void ForgeProcessor::queuePad(int pad, bool on) {
  int a = 0, n = 0, b = 0, m = 0;
  padFifo.prepareToWrite(1, a, n, b, m);
  if (n) padEvents[size_t(a)] = {pad, on};
  padFifo.finishedWrite(n);
}

void ForgeProcessor::startVoice(const forge::Clip *clip, int note, int channel, float velocity, int pad, int cue, int delay) {
  int sounding = 0;
  for (const auto &v : voices) if (v.sounding()) sounding++;
  if (sounding >= kSounding) {   // full: the oldest playing voice fades out
    forge::Voice *oldest = nullptr;
    for (auto &v : voices) if (v.sounding() && (!oldest || v.order < oldest->order)) oldest = &v;
    if (oldest) oldest->kill(hostRate, 5);
  }
  size_t slot = voices.size();
  for (size_t i = 0; i < voices.size(); i++) if (!voices[i].active()) { slot = i; break; }
  if (slot == voices.size()) {   // everything still fading: take the quietest-to-lose (oldest)
    slot = 0;
    for (size_t i = 1; i < voices.size(); i++) if (voices[i].order < voices[slot].order) slot = i;
  }
  auto &v = voices[slot];
  v.start(clip, hostRate, note, channel, velocity);
  v.pad = pad;
  v.cue = cue;
  v.order = ++voiceOrder;
  v.kit = audioKit ? audioKit->generation : 0;
  delays[slot] = delay;
}

void ForgeProcessor::noteEvent(int note, int channel, float velocity, bool on, double ppq, bool playing, double bpm, int sampleOffset,
                               bool fromUi) {
  if (!audioKit) return;
  const int base = padBase();
  const int bank = int(pBank->load());
  if (!on) {
    for (size_t i = 0; i < voices.size(); i++)
      if (voices[i].note == note && voices[i].channel == channel && voices[i].clip && voices[i].clip->mode == 1) {
        if (delays[i] > 0) voices[i].clip = nullptr;
        else voices[i].release();
      }
    return;
  }
  const forge::Clip *clip = nullptr;
  int pad = -1, cue = -1;
  if (audioKit->keyboard && !fromUi) {
    const int index = note - 48 + 24;   // C3 (48) = original pitch
    if (index >= 0 && index < 49) clip = audioKit->keys[size_t(index)].get();
    pad = audioKit->selected;
    cue = audioKit->bank * 16 + audioKit->selected;
  } else {
    const int i = fromUi ? note : note - base;
    if (i >= 0 && i < 16) {
      pad = i;
      cue = bank * 16 + i;
      clip = audioKit->cues[size_t(cue)].get();
      padHits[size_t(i)].fetch_add(1, std::memory_order_relaxed);
      if (!fromUi) lastHitPad.store(i, std::memory_order_relaxed);
    }
  }
  if (!clip) return;
  if (clip->mode == 2)   // loop pads toggle
    for (auto &v : voices)
      if (v.clip == clip && v.note == note && !v.releasing && !v.killing) { v.release(); return; }
  if (pPoly->load() < .5f) {
    for (auto &v : voices)
      if (v.sounding()) v.kill(hostRate);   // mono: one sound at a time
  } else if (clip->choke) {
    for (auto &v : voices)
      if (v.sounding() && v.cue == cue) v.kill(hostRate);   // choke: this pad cuts its own last hit
  }
  int delay = 0;
  const int q = int(pQuantize->load());
  if (q && playing && std::isfinite(ppq)) {
    const double grid = q == 1 ? 1 : q == 2 ? .5 : .25, position = ppq + sampleOffset / hostRate * bpm / 60,
                 target = std::ceil((position + 1e-9) / grid) * grid;
    delay = int(std::round((target - position) * 60 / bpm * hostRate));
  }
  startVoice(clip, note, channel, velocity, pad, cue, delay);
}

void ForgeProcessor::processBlock(juce::AudioBuffer<float> &output, juce::MidiBuffer &midi) {
  juce::ScopedNoDenormals noDenormals;
  output.clear();
  double bpm = hostBpm.load(), ppq = std::numeric_limits<double>::quiet_NaN();
  bool playing = false;
  if (auto *head = getPlayHead())
    if (auto pos = head->getPosition()) {
      if (auto tempo = pos->getBpm()) bpm = *tempo;
      if (auto beat = pos->getPpqPosition()) ppq = *beat;
      playing = pos->getIsPlaying();
    }
  bpm = juce::jlimit(30., 300., bpm);
  hostBpm = bpm;
  hostPlaying = playing;

  // new prepared audio: old voices fade out (no click); their audio stays alive until they're done
  auto ready = std::atomic_load(&published);
  if (ready != audioKit) {
    const uint64_t oldGen = fadingKit ? fadingKit->generation : ~uint64_t(0);
    for (auto &v : voices)
      if (v.active() && v.kit == oldGen) v.clip = nullptr;   // a second swap mid-fade: the oldest ones stop now
    bool any = false;
    for (auto &v : voices) if (v.active()) { v.kill(hostRate, 10); any = true; }
    fadingKit = any ? audioKit : nullptr;
    audioKit = ready;
  }
  if (fadingKit) {
    bool still = false;
    for (const auto &v : voices) if (v.active() && v.kit == fadingKit->generation) still = true;
    if (!still) fadingKit = nullptr;
  }
  if (panic.exchange(false))
    for (auto &v : voices) v.kill(hostRate, 10);

  int a = 0, n = 0, b = 0, m = 0;
  padFifo.prepareToRead(256, a, n, b, m);
  auto pad = [&](int i) {
    const auto e = padEvents[size_t(i)];
    noteEvent(e.pad, 0, .9f, e.on, ppq, playing, bpm, 0, true);   // channel 0: UI pads never clash with MIDI notes
  };
  for (int i = 0; i < n; i++) pad(a + i);
  for (int i = 0; i < m; i++) pad(b + i);
  padFifo.finishedRead(n + m);

  volume.setTargetValue(pMaster->load());
  bus.setFilters(pLowCut->load(), pHighCut->load());
  auto it = midi.begin();
  auto *L = output.getWritePointer(0);
  auto *R = output.getWritePointer(1);
  for (int sample = 0; sample < output.getNumSamples(); sample++) {
    while (it != midi.end() && (*it).samplePosition <= sample) {
      const auto message = (*it).getMessage();
      if (message.isAllNotesOff() || message.isAllSoundOff())
        for (auto &v : voices) v.kill(hostRate);
      else if (message.isNoteOn() && padLearn.load()) {
        padLearned.store(message.getNoteNumber());
        padLearn.store(false);
      } else if (message.isNoteOnOrOff())
        noteEvent(message.getNoteNumber(), message.getChannel(), message.getFloatVelocity(), message.isNoteOn(), ppq, playing, bpm, sample, false);
      ++it;
    }
    float left = 0, right = 0;
    for (size_t i = 0; i < voices.size(); i++) {
      if (delays[i] > 0) { delays[i]--; continue; }
      voices[i].tick(left, right, hostRate);
    }
    const auto gain = volume.getNextValue();
    left *= gain;
    right *= gain;
    bus.process(left, right);
    L[sample] = left;
    R[sample] = right;
  }
  int active = 0, mask = 0;
  const forge::Voice *newest = nullptr;
  for (const auto &v : voices)
    if (v.active()) {
      active++;
      if (v.sounding() && v.pad >= 0 && v.pad < 16) mask |= 1 << v.pad;
      if (v.sounding() && (!newest || v.order > newest->order)) newest = &v;
    }
  activeVoices = active;
  padSounding = mask;
  if (newest && newest->clip) {
    const auto *c = newest->clip;
    const double f = newest->progress();
    playCue = newest->cue;
    playPos = c->reverse ? c->srcEnd - f * (c->srcEnd - c->srcStart) : c->srcStart + f * (c->srcEnd - c->srcStart);
  } else playCue = -1;
  midi.clear();
}

// ---------------------------------------------------------------- saved with the Live set
// "SFN4", project JSON, the sample as 24-bit FLAC (lossless enough, much smaller than raw float)

void ForgeProcessor::getStateInformation(juce::MemoryBlock &dest) {
  juce::MemoryOutputStream out(dest, false);
  out.writeInt(0x53464e34);
  const auto p = project();
  juce::DynamicObject::Ptr root = new juce::DynamicObject();
  root->setProperty("uiScale", uiScale);
  if (p && p->sample) {
    root->setProperty("name", p->sample->name);
    root->setProperty("path", p->sample->path);
    root->setProperty("sourceBpm", p->sourceBpm);
    root->setProperty("sourceKey", p->sourceKey);
    root->setProperty("bpmDetected", p->bpmDetected);
    root->setProperty("keyDetected", p->keyDetected);
    juce::Array<juce::var> cues;
    for (const auto &q : p->cues) {
      if (!q.present) { cues.add(juce::var()); continue; }
      juce::DynamicObject::Ptr c = new juce::DynamicObject();
      c->setProperty("start", q.start); c->setProperty("end", q.end); c->setProperty("pitch", q.pitch);
      c->setProperty("gain", q.gain); c->setProperty("cutoff", q.cutoff); c->setProperty("attack", q.attack);
      c->setProperty("release", q.release); c->setProperty("reverse", q.reverse); c->setProperty("mode", q.mode);
      c->setProperty("choke", q.choke); c->setProperty("name", juce::String(q.name));
      cues.add(juce::var(c.get()));
    }
    root->setProperty("cues", cues);
  }
  out.writeString(juce::JSON::toString(juce::var(root.get()), true));
  out.writeString(parameters.copyState().createXml()->toString());
  // audio
  if (p && p->sample) {
    if (flacFor != p->sample) {
      flacCache.reset();
      juce::FlacAudioFormat flac;
      if (std::unique_ptr<juce::AudioFormatWriter> w{flac.createWriterFor(new juce::MemoryOutputStream(flacCache, false), p->sample->rate, 2, 24, {}, 0)}) {
        std::array<std::vector<float>, 2> clipped = p->sample->audio;
        for (auto &ch : clipped) for (auto &x : ch) x = juce::jlimit(-1.f, 1.f, x);
        const float *chans[2] = {clipped[0].data(), clipped[1].data()};
        w->writeFromFloatArrays(chans, 2, int(clipped[0].size()));
      }
      flacFor = p->sample;
    }
    out.writeInt64(juce::int64(flacCache.getSize()));
    out.write(flacCache.getData(), flacCache.getSize());
  } else out.writeInt64(0);
}

void ForgeProcessor::setStateInformation(const void *data, int size) {
  if (size < 8) return;
  juce::MemoryInputStream in(data, size_t(size), false);
  const int magic = in.readInt();
  if (magic == 0x53464e32) {   // v0.3: the .sforge project + parameters
    const int count = in.readInt();
    if (count < 0 || count > in.getNumBytesRemaining()) return;
    if (count) {
      juce::MemoryBlock bytes;
      in.readIntoMemoryBlock(bytes, count);
      if (loadSforge(bytes, false).failed()) return;
    }
    if (auto xml = juce::parseXML(in.readString())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
    panic = true;
    return;
  }
  if (magic != 0x53464e34) return;
  const auto root = juce::JSON::parse(in.readString());
  const auto xmlText = in.readString();
  const juce::int64 flacSize = in.readInt64();
  uiScale = float(double(root.getProperty("uiScale", 0.0)));
  if (auto xml = juce::parseXML(xmlText)) parameters.replaceState(juce::ValueTree::fromXml(*xml));
  if (flacSize <= 0 || flacSize > in.getNumBytesRemaining()) return;
  juce::MemoryBlock flacBytes;
  in.readIntoMemoryBlock(flacBytes, size_t(flacSize));
  juce::FlacAudioFormat flac;
  std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(new juce::MemoryInputStream(flacBytes, false), true));
  if (!reader) return;
  const int n = int(reader->lengthInSamples);
  juce::AudioBuffer<float> buf(2, n);
  reader->read(&buf, 0, n, 0, true, true);
  std::array<std::vector<float>, 2> audio;
  for (int c = 0; c < 2; c++) audio[size_t(c)].assign(buf.getReadPointer(c), buf.getReadPointer(c) + n);
  auto p = std::make_shared<ForgeProject>();
  p->sample = analyse(std::move(audio), reader->sampleRate, root["name"].toString(), root["path"].toString());
  p->sourceBpm = juce::jlimit(30.0, 300.0, double(root.getProperty("sourceBpm", 120.0)));
  p->sourceKey = juce::jlimit(-1, 23, int(root.getProperty("sourceKey", -1)));
  p->bpmDetected = bool(root.getProperty("bpmDetected", false));
  p->keyDetected = bool(root.getProperty("keyDetected", false));
  if (const auto *cues = root["cues"].getArray())
    for (int i = 0; i < juce::jmin(64, cues->size()); i++) {
      const auto &v = cues->getReference(i);
      if (!v.isObject()) continue;
      auto &q = p->cues[size_t(i)];
      q.present = true;
      q.start = double(v["start"]); q.end = double(v["end"]); q.pitch = float(v["pitch"]); q.gain = float(v["gain"]);
      q.cutoff = float(v["cutoff"]); q.attack = float(v["attack"]); q.release = float(v["release"]); q.reverse = bool(v["reverse"]);
      q.mode = juce::jlimit(0, 2, int(v["mode"])); q.choke = bool(v["choke"]); q.name = v["name"].toString().toStdString();
      q.end = juce::jlimit(q.start + .001, std::max(q.start + .001, p->sample->length()), q.end);
    }
  flacCache = flacBytes;   // no need to re-encode on the next save
  flacFor = p->sample;
  {
    const juce::ScopedLock lock(stateLock);
    p->version = ++versionCounter;
  }
  setProject(p, "Restored " + p->sample->name);
  panic = true;
}

juce::AudioProcessorEditor *ForgeProcessor::createEditor() { return new ForgeEditor(*this); }
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new ForgeProcessor(); }
