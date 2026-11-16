#pragma once
#include "PluginProcessor.h"

namespace sfui {
const juce::Colour bg{0xff131417}, panel{0xff1c1e22}, panel2{0xff24272c}, line{0xff34383f}, text{0xffe9e6e1},
    muted{0xff8b9099}, accent{0xffff7447}, wave{0xffef956d};
juce::Colour padColour(int pad);
} // namespace sfui

class JaySampleView;

// Overview strip + zoomable waveform. Cues of the current bank as coloured regions; drag the selected cue's edges.
class WaveView : public juce::Component {
public:
  WaveView(ForgeProcessor &p, JaySampleView &v) : proc(p), view(v) {}
  void paint(juce::Graphics &) override;
  void mouseDown(const juce::MouseEvent &) override;
  void mouseDrag(const juce::MouseEvent &) override;
  void mouseUp(const juce::MouseEvent &) override;
  void mouseDoubleClick(const juce::MouseEvent &) override;
  void mouseWheelMove(const juce::MouseEvent &, const juce::MouseWheelDetails &) override;
  void zoom(double factor, double aroundSeconds);
  void fit();
  int snapMode = 1;   // 0 off, 1 zero crossing, 2 beat grid
private:
  juce::Rectangle<int> overviewArea() const { return getLocalBounds().removeFromTop(34); }
  juce::Rectangle<int> mainArea() const { return getLocalBounds().withTrimmedTop(40); }
  double xToTime(float x) const;
  float timeToX(double t) const;
  double snap(double t) const;
  void drawWave(juce::Graphics &, juce::Rectangle<int>, double from, double to, juce::Colour) const;
  void clampView();
  ForgeProcessor &proc;
  JaySampleView &view;
  double viewStart = 0, viewEnd = 0;
  const SampleData *viewFor = nullptr;
  enum Drag { None, Start, End, Pan, Overview } drag = None;
  double dragStartT = 0, dragEndT = 0, panAnchor = 0, panStart = 0, panEnd = 0;
};

// 4x4 pads (pad 1 bottom-left, like a pad controller). Lit while playing, flash on hits, outline = selected.
class PadGrid : public juce::Component {
public:
  PadGrid(ForgeProcessor &p, JaySampleView &v) : proc(p), view(v) {}
  void paint(juce::Graphics &) override;
  void mouseDown(const juce::MouseEvent &) override;
  void mouseUp(const juce::MouseEvent &) override;
  std::array<double, 16> flashUntil{};
private:
  int padAt(juce::Point<float>) const;
  juce::Rectangle<float> padRect(int pad) const;
  ForgeProcessor &proc;
  JaySampleView &view;
  int pressed = -1;
};

// drag the selected cue (rendered at the current key / tempo) onto a Live track
class DragTile : public juce::Component {
public:
  explicit DragTile(ForgeProcessor &p, JaySampleView &v) : proc(p), view(v) {}
  void paint(juce::Graphics &) override;
  void mouseDrag(const juce::MouseEvent &) override;
  void mouseUp(const juce::MouseEvent &) override { dragging = false; }
  juce::String message{"DRAG THIS PAD TO A LIVE TRACK"};
private:
  ForgeProcessor &proc;
  JaySampleView &view;
  bool dragging = false;
};

class JaySampleView : public juce::Component, public juce::FileDragAndDropTarget, private juce::Timer {
public:
  static constexpr int W = 1120, H = 780;
  explicit JaySampleView(ForgeProcessor &);
  ~JaySampleView() override;
  void paint(juce::Graphics &) override;
  void paintOverChildren(juce::Graphics &) override;
  void resized() override;
  bool isInterestedInFileDrag(const juce::StringArray &) override;
  void fileDragEnter(const juce::StringArray &, int, int) override { dropHover = true; repaint(); }
  void fileDragExit(const juce::StringArray &) override { dropHover = false; repaint(); }
  void filesDropped(const juce::StringArray &, int, int) override;
  std::function<void(float)> onScale;

  int bank() const { return juce::jlimit(0, 3, int(proc.parameters.getRawParameterValue("bank")->load())); }
  int selectedPad() const { return juce::jlimit(0, 15, int(proc.parameters.getRawParameterValue("cue")->load())); }
  int selectedCue() const { return bank() * 16 + selectedPad(); }
  void selectPad(int pad);
  void setParam(const char *id, float v);
  void openFile(const juce::File &);

private:
  void timerCallback() override;
  void syncCuePanel(bool force);
  void editSelected(const std::function<void(forge::Cue &)> &fn) { proc.editCue(selectedCue(), fn); }
  juce::Slider &knob(juce::Slider &, juce::Label &, const juce::String &name, double lo, double hi, double step, const juce::String &suffix);
  juce::Slider &bar(juce::Slider &, juce::Label &, const juce::String &name);
  ForgeProcessor &proc;
  juce::TooltipWindow tooltips{this, 600};

  juce::TextButton loadButton{"LOAD"}, stopButton{"STOP"};
  juce::Label nameLabel, statusLabel;
  juce::ComboBox sizeBox;
  std::unique_ptr<juce::FileChooser> chooser;

  WaveView wave{proc, *this};
  juce::TextButton findButton{"FIND SAMPLES"}, rerollButton{"RE-ROLL"}, sliceButton{"SLICE"}, zoomIn{"+"}, zoomOut{"-"}, zoomFit{"FIT"};
  juce::Slider sensSlider;
  juce::Label sensLabel, waveHint;
  juce::ComboBox sliceBox, snapBox;

  std::array<juce::TextButton, 4> bankButtons;
  PadGrid pads{proc, *this};

  // cue panel
  juce::Label cueTitle;
  juce::TextEditor cueName;
  juce::ComboBox modeBox;
  juce::Slider pitchK, gainK, attackK, releaseK, filterK;
  juce::Label pitchL, gainL, attackL, releaseL, filterL;
  juce::ToggleButton reverseT{"Reverse"}, chokeT{"Choke"};
  juce::TextButton clearCueButton{"CLEAR PAD"};
  DragTile dragTile{proc, *this};
  bool syncing = false;
  int shownCue = -1;
  uint64_t shownVersion = 0;

  // key + tempo + output
  juce::Label srcKeyL, srcBpmL, shiftL, targetKeyL, tbpmL, quantL, masterL, lowL, highL, padKeysL, padRangeL;
  juce::ComboBox srcKeyBox, quantBox;
  juce::Slider srcBpm, shiftS, tbpmS, masterS, lowS, highS;
  juce::TextButton halfButton{"1/2"}, doubleButton{"x2"}, origButton{"ORIG"}, padDown{"<"}, padUp{">"}, learnButton{"LEARN"};
  juce::ToggleButton syncT{"Sync to Live"}, formantsT{"Formants"}, polyT{"Poly"}, keyboardT{"Keyboard mode"}, followT{"Follow"};
  using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
  using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
  using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
  std::unique_ptr<SA> shiftA, tbpmA, masterA, lowA, highA;
  std::unique_ptr<BA> syncA, formantsA, polyA, keyboardA;
  std::unique_ptr<CA> quantA;

  juce::Rectangle<int> headerR, waveR, padsR, cueR, keyR;
  std::array<int, 16> seenHits{};
  bool dropHover = false;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(JaySampleView)
};

// Holds the view and scales it (100 / 85 / 70 %). First open: the biggest size that fits the screen.
class ForgeEditor final : public juce::AudioProcessorEditor {
public:
  explicit ForgeEditor(ForgeProcessor &p) : AudioProcessorEditor(p), proc(p), view(p) {
    addAndMakeVisible(view);
    view.onScale = [this](float s) { applyScale(s); };
    applyScale(proc.uiScale > 0 ? proc.uiScale : fitScale());
  }
  void resized() override {}
private:
  static float fitScale() {
    if (auto *d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()) {
      const float h = float(d->userArea.getHeight()) - 90.f, w = float(d->userArea.getWidth()) - 40.f;
      for (float s : {1.f, .85f, .7f})
        if (JaySampleView::H * s <= h && JaySampleView::W * s <= w) return s;
    }
    return .7f;
  }
  void applyScale(float s) {
    s = juce::jlimit(.6f, 1.f, s);
    proc.uiScale = s;
    view.setBounds(0, 0, JaySampleView::W, JaySampleView::H);
    view.setTransform(juce::AffineTransform::scale(s));
    setSize(juce::roundToInt(JaySampleView::W * s), juce::roundToInt(JaySampleView::H * s));
  }
  ForgeProcessor &proc;
  JaySampleView view;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ForgeEditor)
};
