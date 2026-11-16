#pragma once
#include "ForgeDSP.h"
#include "ForgeAnalysis.h"
#include <JuceHeader.h>

// The loaded sample: audio plus everything worked out from it once (never changes after loading)
struct SampleData {
  std::array<std::vector<float>, 2> audio;
  double rate = 44100;
  juce::String name, path;
  std::vector<float> mono;
  forge::Peaks peaks;
  std::vector<forge::Onset> onsets;
  double length() const { return rate > 0 ? double(audio[0].size()) / rate : 0; }
};

// A project: the sample + 64 cues (4 banks x 16) + its key / tempo. Edits make a new copy (the audio is shared).
struct ForgeProject {
  std::shared_ptr<const SampleData> sample;
  std::array<forge::Cue, 64> cues;
  double sourceBpm = 120;
  int sourceKey = -1;          // 0-11 major, 12-23 minor, -1 unknown
  bool bpmDetected = false, keyDetected = false;
  uint64_t version = 0;
};

// Prepared (pitched / stretched) audio for every cue, built on the renderer thread
struct ForgeKit {
  std::array<std::shared_ptr<const forge::Clip>, 64> cues;
  std::array<std::shared_ptr<const forge::Clip>, 49> keys;   // keyboard mode: selected cue, -24..+24 st
  int bank = 0, selected = 0;
  bool keyboard = false;
  double targetBpm = 120;
  uint64_t generation = 0;
};

class ForgeProcessor final : public juce::AudioProcessor, private juce::Timer, private juce::Thread {
public:
  ForgeProcessor();
  ~ForgeProcessor() override;
  juce::AudioProcessorValueTreeState parameters;
  void prepareToPlay(double, int) override;
  void releaseResources() override;
  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
  using juce::AudioProcessor::processBlock;
  bool isBusesLayoutSupported(const BusesLayout &) const override;
  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override { return true; }
  const juce::String getName() const override { return "JaySample"; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 2; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return "Default"; }
  void changeProgramName(int, const juce::String &) override {}
  void getStateInformation(juce::MemoryBlock &) override;
  void setStateInformation(const void *, int) override;

  // ---- loading (message thread). Audio files load + analyse in the background.
  void loadAudioFile(const juce::File &);
  juce::Result loadSforge(const juce::MemoryBlock &, bool setDefaults = true);
  bool isLoading() const { return loading.load(); }
  static bool isAudioFile(const juce::File &f) { return f.hasFileExtension("wav;aif;aiff;flac;mp3;m4a;ogg;caf"); }

  // ---- project + edits (message thread)
  std::shared_ptr<const ForgeProject> project() const;
  void editProject(const std::function<void(ForgeProject &)> &);
  void editCue(int index, const std::function<void(forge::Cue &)> &);
  void findSamples(int bank, float sensitivity, bool reroll);
  void sliceEvenly(int bank, int count);
  void addCueAt(int index, double seconds);   // a cue starting here, ending at the next hit (or +1 s)
  juce::File exportCue(int index, juce::String &message);   // rendered WAV, for drag and drop
  juce::String status() const;
  float sensitivity = .5f;

  // ---- live info for the UI
  std::atomic<double> hostBpm{120};
  std::atomic<bool> hostPlaying{false};
  std::atomic<bool> panic{false};
  std::atomic<int> activeVoices{0};
  std::array<std::atomic<int>, 16> padHits{};   // count up per hit (lights flash)
  std::atomic<int> padSounding{0};              // bit per pad (current bank) that's playing
  std::atomic<int> lastHitPad{-1};              // controller hits select the pad (Follow)
  std::atomic<int> playCue{-1};                 // cue of the newest voice, -1 none
  std::atomic<double> playPos{0};               // where it is in the sample (seconds)
  void queuePad(int pad, bool on);              // UI pad presses
  // PAD KEYS / LEARN
  int padBase() const { return juce::jlimit(0, 112, int(pPadBase->load())); }
  std::atomic<bool> padLearn{false};
  std::atomic<int> padLearned{-1};
  float uiScale = 0;   // window size, saved with the set (0 = fit the screen)

private:
  static juce::AudioProcessorValueTreeState::ParameterLayout layout();
  void timerCallback() override;
  void run() override;
  void noteEvent(int note, int channel, float velocity, bool on, double ppq, bool playing, double bpm, int sampleOffset, bool fromUi);
  void setProject(std::shared_ptr<const ForgeProject>, const juce::String &statusMessage);
  static std::shared_ptr<SampleData> analyse(std::array<std::vector<float>, 2> &&audio, double rate, const juce::String &name,
                                             const juce::String &path);
  static void autoCues(ForgeProject &, int bank, float sensitivity, uint32_t seed);
  void startVoice(const forge::Clip *, int note, int channel, float velocity, int pad, int cue, int delay);

  mutable juce::CriticalSection stateLock;
  std::shared_ptr<const ForgeProject> source;
  juce::String statusText = "Drop a sample (WAV, AIFF, MP3, FLAC) or a .sforge project";
  uint64_t versionCounter = 0;
  struct Job {
    std::shared_ptr<const ForgeProject> project;
    double tempo = 1, target = 120;
    float transpose = 0;
    bool formants = false, keyboard = false;
    int bank = 0, selected = 0;
    uint64_t generation = 0;
  };
  Job requested;
  uint64_t nextGeneration = 0;
  juce::String lastSignature;
  juce::WaitableEvent wake;
  std::shared_ptr<const ForgeKit> published, audioKit, fadingKit;
  std::vector<std::shared_ptr<const ForgeKit>> retained;
  std::map<std::string, std::shared_ptr<const forge::Clip>> clipCache;   // renderer thread only
  juce::ThreadPool loader{1};
  std::atomic<bool> loading{false};
  std::shared_ptr<int> alive = std::make_shared<int>(0);
  juce::MemoryBlock flacCache;                 // the sample as FLAC, for the saved state (made once per sample)
  std::shared_ptr<const SampleData> flacFor;

  static constexpr int kVoices = 40, kSounding = 32;   // 32 playing + room for fading-out ones
  std::array<forge::Voice, kVoices> voices;
  std::array<int, kVoices> delays{};
  uint64_t voiceOrder = 0;
  double hostRate = 44100;
  juce::LinearSmoothedValue<float> volume;
  forge::MasterBus bus;
  struct PadEvent { int pad = 0; bool on = false; };
  juce::AbstractFifo padFifo{256};
  std::array<PadEvent, 256> padEvents;
  std::atomic<float> *pMaster = nullptr, *pBpm = nullptr, *pTranspose = nullptr, *pBank = nullptr, *pCue = nullptr,
                     *pSync = nullptr, *pKeyboard = nullptr, *pPoly = nullptr, *pFormants = nullptr, *pQuantize = nullptr,
                     *pLowCut = nullptr, *pHighCut = nullptr, *pPadBase = nullptr;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeProcessor)
};
