#include "PluginEditor.h"

using namespace sfui;

juce::Colour sfui::padColour(int pad) {
  static const juce::uint32 c[4] = {0xffff7447, 0xff4fc3c9, 0xffe8c547, 0xffb07cf0};   // a colour per pad row
  return juce::Colour(c[(pad / 4) % 4]).withRotatedHue(float(pad % 4) * .03f);
}

// ================================================================ waveform

double WaveView::xToTime(float x) const {
  const auto r = mainArea();
  return viewStart + (viewEnd - viewStart) * double(x - float(r.getX())) / std::max(1.0, double(r.getWidth()));
}

float WaveView::timeToX(double t) const {
  const auto r = mainArea();
  return float(r.getX()) + float((t - viewStart) / std::max(1e-9, viewEnd - viewStart) * r.getWidth());
}

double WaveView::snap(double t) const {
  auto p = proc.project();
  if (!p || !p->sample) return t;
  if (snapMode == 1) return forge::snapToZero(p->sample->mono, p->sample->rate, t);
  if (snapMode == 2) return forge::snapToBeat(t, p->sourceBpm);
  return t;
}

void WaveView::clampView() {
  auto p = proc.project();
  const double len = p && p->sample ? p->sample->length() : 1;
  double w = juce::jlimit(.02, std::max(.02, len), viewEnd - viewStart);
  viewStart = juce::jlimit(0.0, std::max(0.0, len - w), viewStart);
  viewEnd = viewStart + w;
}

void WaveView::fit() {
  auto p = proc.project();
  viewStart = 0;
  viewEnd = p && p->sample ? p->sample->length() : 1;
  repaint();
}

void WaveView::zoom(double factor, double around) {
  const double w = (viewEnd - viewStart) * factor;
  const double f = (around - viewStart) / std::max(1e-9, viewEnd - viewStart);
  viewStart = around - f * w;
  viewEnd = viewStart + w;
  clampView();
  repaint();
}

void WaveView::drawWave(juce::Graphics &g, juce::Rectangle<int> r, double from, double to, juce::Colour c) const {
  auto p = proc.project();
  if (!p || !p->sample) return;
  const auto &s = *p->sample;
  const float mid = float(r.getCentreY()), half = float(r.getHeight()) * .46f;
  const double perPx = (to - from) * s.rate / std::max(1, r.getWidth());
  g.setColour(c);
  for (int x = 0; x < r.getWidth(); x++) {
    const double a = (from + (to - from) * x / r.getWidth()) * s.rate, b = a + perPx;
    float lo = 0, hi = 0;
    if (perPx >= s.peaks.block) {   // zoomed out: the precomputed min / max
      const size_t i0 = size_t(std::max(0.0, a / s.peaks.block)), i1 = std::min(s.peaks.lo.size(), size_t(b / s.peaks.block) + 1);
      for (size_t i = i0; i < i1; i++) { lo = std::min(lo, s.peaks.lo[i]); hi = std::max(hi, s.peaks.hi[i]); }
    } else {
      const size_t i0 = size_t(std::max(0.0, a)), i1 = std::min(s.mono.size(), size_t(std::max(a + 1, b)) + 1);
      for (size_t i = i0; i < i1; i++) { lo = std::min(lo, s.mono[i]); hi = std::max(hi, s.mono[i]); }
    }
    g.drawVerticalLine(r.getX() + x, mid - hi * half, mid - lo * half + 1);
  }
}

void WaveView::paint(juce::Graphics &g) {
  auto p = proc.project();
  const auto ov = overviewArea(), main = mainArea();
  g.setColour(panel2);
  g.fillRoundedRectangle(ov.toFloat(), 4);
  g.fillRoundedRectangle(main.toFloat(), 4);
  if (!p || !p->sample) {
    g.setColour(muted);
    g.setFont(juce::Font(juce::FontOptions(16.f)));
    g.drawFittedText(proc.isLoading() ? "Loading and analysing ..." : "Drop a sample here (WAV, AIFF, MP3, FLAC) or press LOAD\nKey, BPM and 16 samples are found for you",
                     main, juce::Justification::centred, 2);
    return;
  }
  if (viewFor != p->sample.get()) { viewFor = p->sample.get(); fit(); }
  const double len = p->sample->length();
  const int bank = view.bank(), sel = view.selectedCue();
  // overview: whole sample, cue ticks, the zoomed window
  drawWave(g, ov.reduced(2), 0, len, wave.withAlpha(.7f));
  for (int i = 0; i < 16; i++) {
    const auto &q = p->cues[size_t(bank * 16 + i)];
    if (!q.present) continue;
    const float x = float(ov.getX()) + float(q.start / len * ov.getWidth());
    g.setColour(padColour(i));
    g.fillRect(x, float(ov.getY()), 2.f, float(ov.getHeight()));
  }
  {
    const float x0 = float(ov.getX()) + float(viewStart / len * ov.getWidth()), x1 = float(ov.getX()) + float(viewEnd / len * ov.getWidth());
    g.setColour(juce::Colours::white.withAlpha(.12f));
    g.fillRect(x0, float(ov.getY()), std::max(2.f, x1 - x0), float(ov.getHeight()));
    g.setColour(juce::Colours::white.withAlpha(.5f));
    g.drawRect(x0, float(ov.getY()), std::max(2.f, x1 - x0), float(ov.getHeight()), 1.f);
  }
  // main view: cue regions, then the waveform on top
  g.saveState();
  g.reduceClipRegion(main);
  for (int i = 0; i < 16; i++) {
    const int ci = bank * 16 + i;
    const auto &q = p->cues[size_t(ci)];
    if (!q.present) continue;
    double s0 = q.start, s1 = q.end;
    if (ci == sel && drag != None && drag != Pan && drag != Overview) { s0 = dragStartT; s1 = dragEndT; }
    const float x0 = timeToX(s0), x1 = timeToX(s1);
    if (x1 < main.getX() || x0 > main.getRight()) continue;
    g.setColour(padColour(i).withAlpha(ci == sel ? .28f : .12f));
    g.fillRect(x0, float(main.getY()), x1 - x0, float(main.getHeight()));
    g.setColour(padColour(i).withAlpha(ci == sel ? 1.f : .6f));
    g.fillRect(x0, float(main.getY()), 2.f, float(main.getHeight()));
    g.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
    g.drawText(juce::String(i + 1), juce::Rectangle<float>(x0 + 4, float(main.getY()) + 2, 30, 14), juce::Justification::left);
    if (ci == sel) {   // handles
      g.setColour(juce::Colours::white);
      g.fillRect(x0 - 1, float(main.getY()), 3.f, float(main.getHeight()));
      g.fillRect(x1 - 2, float(main.getY()), 3.f, float(main.getHeight()));
      g.fillRect(x0 - 4, float(main.getBottom()) - 10, 9.f, 10.f);
      g.fillRect(x1 - 5, float(main.getBottom()) - 10, 9.f, 10.f);
    }
  }
  drawWave(g, main.reduced(0, 6), viewStart, viewEnd, wave);
  // playhead
  if (const int pc = proc.playCue.load(); pc >= 0 && pc / 16 == bank) {
    const float x = timeToX(proc.playPos.load());
    g.setColour(juce::Colours::white);
    g.drawVerticalLine(int(x), float(main.getY()), float(main.getBottom()));
  }
  g.restoreState();
  g.setColour(muted);
  g.setFont(juce::Font(juce::FontOptions(11.f)));
  g.drawText(juce::String(viewStart, 2) + " s", main.reduced(6, 2).removeFromBottom(14), juce::Justification::bottomLeft);
  g.drawText(juce::String(viewEnd, 2) + " s", main.reduced(6, 2).removeFromBottom(14), juce::Justification::bottomRight);
}

void WaveView::mouseDown(const juce::MouseEvent &e) {
  auto p = proc.project();
  if (!p || !p->sample) return;
  if (overviewArea().contains(e.getPosition())) {
    drag = Overview;
    mouseDrag(e);
    return;
  }
  const int sel = view.selectedCue();
  const auto &q = p->cues[size_t(sel)];
  if (q.present) {
    const float xs = timeToX(q.start), xe = timeToX(q.end);
    if (std::abs(e.position.x - xs) < 7) { drag = Start; dragStartT = q.start; dragEndT = q.end; return; }
    if (std::abs(e.position.x - xe) < 7) { drag = End; dragStartT = q.start; dragEndT = q.end; return; }
  }
  // click a cue region: select that pad
  const double t = xToTime(e.position.x);
  for (int i = 15; i >= 0; i--) {
    const auto &c = p->cues[size_t(view.bank() * 16 + i)];
    if (c.present && t >= c.start && t <= c.end) { view.selectPad(i); break; }
  }
  drag = Pan;
  panAnchor = t;
  panStart = viewStart;
  panEnd = viewEnd;
}

void WaveView::mouseDrag(const juce::MouseEvent &e) {
  auto p = proc.project();
  if (!p || !p->sample) return;
  if (drag == Overview) {
    const auto ov = overviewArea();
    const double t = p->sample->length() * double(e.position.x - ov.getX()) / std::max(1, ov.getWidth());
    const double w = viewEnd - viewStart;
    viewStart = t - w / 2;
    viewEnd = viewStart + w;
    clampView();
  } else if (drag == Start) dragStartT = juce::jlimit(0.0, dragEndT - .005, snap(xToTime(e.position.x)));
  else if (drag == End) dragEndT = juce::jlimit(dragStartT + .005, p->sample->length(), snap(xToTime(e.position.x)));
  else if (drag == Pan && e.getDistanceFromDragStartX() != 0) {
    const double dt = (viewEnd - viewStart) * e.getDistanceFromDragStartX() / std::max(1, mainArea().getWidth());
    viewStart = panStart - dt;
    viewEnd = panEnd - dt;
    clampView();
  }
  repaint();
}

void WaveView::mouseUp(const juce::MouseEvent &) {
  if (drag == Start || drag == End) {
    const double s = dragStartT, en = dragEndT;
    proc.editCue(view.selectedCue(), [s, en](forge::Cue &q) { q.start = s; q.end = en; });
  }
  drag = None;
  repaint();
}

void WaveView::mouseDoubleClick(const juce::MouseEvent &e) {
  if (!mainArea().contains(e.getPosition()) || !proc.project() || !proc.project()->sample) return;
  proc.addCueAt(view.selectedCue(), snap(xToTime(e.position.x)));   // double-click: the selected pad starts here
}

void WaveView::mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &w) {
  if (!proc.project() || !proc.project()->sample) return;
  if (std::abs(w.deltaX) > std::abs(w.deltaY)) {   // sideways: scroll
    const double dt = -(viewEnd - viewStart) * w.deltaX * .5;
    viewStart += dt; viewEnd += dt;
    clampView();
    repaint();
    return;
  }
  zoom(w.deltaY > 0 ? .8 : 1.25, xToTime(e.position.x));
}

// ================================================================ pads

juce::Rectangle<float> PadGrid::padRect(int pad) const {
  const float w = float(getWidth()) / 4, h = float(getHeight()) / 4;
  const int col = pad % 4, row = 3 - pad / 4;   // pad 1 bottom-left
  return {col * w + 3, row * h + 3, w - 6, h - 6};
}

int PadGrid::padAt(juce::Point<float> pt) const {
  for (int i = 0; i < 16; i++) if (padRect(i).contains(pt)) return i;
  return -1;
}

void PadGrid::paint(juce::Graphics &g) {
  auto p = proc.project();
  const int bank = view.bank(), sel = view.selectedPad(), mask = proc.padSounding.load(), base = proc.padBase();
  const double now = juce::Time::getMillisecondCounterHiRes();
  for (int i = 0; i < 16; i++) {
    const auto r = padRect(i);
    const bool has = p && p->cues[size_t(bank * 16 + i)].present;
    const bool lit = has && (((mask >> i) & 1) || now < flashUntil[size_t(i)] || pressed == i);
    const auto c = padColour(i);
    g.setColour(!has ? panel2 : lit ? c : c.withAlpha(.22f));
    g.fillRoundedRectangle(r, 6);
    if (i == sel) {
      g.setColour(juce::Colours::white);
      g.drawRoundedRectangle(r.reduced(1), 6, 2);
    }
    g.setColour(lit ? bg : has ? text : muted);
    g.setFont(juce::Font(juce::FontOptions(15.f)).boldened());
    g.drawText(juce::String(i + 1), r.reduced(8, 6), juce::Justification::topLeft);
    g.setFont(juce::Font(juce::FontOptions(10.5f)));
    g.drawText(juce::MidiMessage::getMidiNoteName(base + i, true, true, 3), r.reduced(8, 6), juce::Justification::topRight);
    g.setFont(juce::Font(juce::FontOptions(11.5f)));
    g.drawFittedText(has ? juce::String(p->cues[size_t(bank * 16 + i)].name) : juce::String("empty"), r.reduced(8, 6).toNearestInt(),
                     juce::Justification::bottomLeft, 2);
  }
}

void PadGrid::mouseDown(const juce::MouseEvent &e) {
  pressed = padAt(e.position);
  if (pressed < 0) return;
  view.selectPad(pressed);
  proc.queuePad(pressed, true);
  repaint();
}

void PadGrid::mouseUp(const juce::MouseEvent &) {
  if (pressed >= 0) proc.queuePad(pressed, false);
  pressed = -1;
  repaint();
}

// ================================================================ drag tile

void DragTile::paint(juce::Graphics &g) {
  auto r = getLocalBounds().toFloat().reduced(1);
  g.setColour(panel2);
  g.fillRoundedRectangle(r, 6);
  g.setColour(accent.withAlpha(.7f));
  g.drawRoundedRectangle(r, 6, 1.2f);
  g.setColour(text);
  g.setFont(juce::Font(juce::FontOptions(11.5f)).boldened());
  g.drawFittedText(message, getLocalBounds().reduced(8, 2), juce::Justification::centred, 2);
}

void DragTile::mouseDrag(const juce::MouseEvent &e) {
  if (dragging || e.getDistanceFromDragStart() < 5) return;
  dragging = true;
  juce::String msg;
  const auto f = proc.exportCue(view.selectedCue(), msg);
  message = msg;
  repaint();
  if (f.existsAsFile())
    juce::DragAndDropContainer::performExternalDragDropOfFiles(juce::StringArray(f.getFullPathName()), false, this, [this] { dragging = false; });
  else dragging = false;
}

// ================================================================ main view

juce::Slider &JaySampleView::knob(juce::Slider &s, juce::Label &l, const juce::String &name, double lo, double hi, double step,
                                    const juce::String &suffix) {
  s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
  s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 70, 16);
  s.setRange(lo, hi, step);
  s.setTextValueSuffix(suffix);
  s.setColour(juce::Slider::rotarySliderFillColourId, accent);
  s.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
  addAndMakeVisible(s);
  l.setText(name, juce::dontSendNotification);
  l.setJustificationType(juce::Justification::centred);
  l.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
  l.setColour(juce::Label::textColourId, muted);
  addAndMakeVisible(l);
  return s;
}

juce::Slider &JaySampleView::bar(juce::Slider &s, juce::Label &l, const juce::String &name) {
  s.setSliderStyle(juce::Slider::LinearBar);
  s.setColour(juce::Slider::trackColourId, accent.withAlpha(.55f));
  s.setColour(juce::Slider::backgroundColourId, panel2);
  s.setColour(juce::Slider::textBoxTextColourId, text);
  s.setColour(juce::Slider::textBoxOutlineColourId, line);
  addAndMakeVisible(s);
  l.setText(name, juce::dontSendNotification);
  l.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
  l.setColour(juce::Label::textColourId, muted);
  addAndMakeVisible(l);
  return s;
}

void JaySampleView::setParam(const char *id, float v) {
  if (auto *prm = proc.parameters.getParameter(id)) {
    prm->beginChangeGesture();
    prm->setValueNotifyingHost(prm->convertTo0to1(v));
    prm->endChangeGesture();
  }
}

void JaySampleView::selectPad(int pad) {
  setParam("cue", float(juce::jlimit(0, 15, pad)));
  syncCuePanel(true);
  pads.repaint();
  wave.repaint();
}

void JaySampleView::openFile(const juce::File &f) {
  if (f.hasFileExtension("sforge")) {
    juce::MemoryBlock bytes;
    if (f.getSize() > 512ll * 1024 * 1024 || !f.loadFileAsData(bytes)) { statusLabel.setText("Can't read " + f.getFileName(), juce::dontSendNotification); return; }
    auto r = proc.loadSforge(bytes);
    if (r.failed()) statusLabel.setText(r.getErrorMessage(), juce::dontSendNotification);
  } else if (ForgeProcessor::isAudioFile(f)) proc.loadAudioFile(f);
}

JaySampleView::JaySampleView(ForgeProcessor &p) : proc(p) {
  auto &lf = getLookAndFeel();
  lf.setColour(juce::ResizableWindow::backgroundColourId, bg);
  lf.setColour(juce::TextButton::buttonColourId, panel2);
  lf.setColour(juce::TextButton::textColourOffId, text);
  lf.setColour(juce::ComboBox::backgroundColourId, panel2);
  lf.setColour(juce::ComboBox::outlineColourId, line);
  lf.setColour(juce::ComboBox::textColourId, text);
  lf.setColour(juce::PopupMenu::backgroundColourId, panel);
  lf.setColour(juce::ToggleButton::textColourId, text);
  lf.setColour(juce::ToggleButton::tickColourId, accent);
  lf.setColour(juce::Label::textColourId, text);

  // header
  for (auto *b : {&loadButton, &stopButton}) addAndMakeVisible(*b);
  loadButton.setTooltip("Load a sample (WAV, AIFF, MP3, FLAC, M4A) or a .sforge project. You can also drop files on the window");
  loadButton.onClick = [this] {
    chooser = std::make_unique<juce::FileChooser>("Load a sample", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.caf;*.sforge");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser &fc) {
      if (fc.getResult().existsAsFile()) openFile(fc.getResult());
    });
  };
  stopButton.setTooltip("Stop everything (fades out)");
  stopButton.onClick = [this] { proc.panic = true; };
  nameLabel.setFont(juce::Font(juce::FontOptions(15.f)).boldened());
  statusLabel.setFont(juce::Font(juce::FontOptions(12.f)));
  statusLabel.setColour(juce::Label::textColourId, muted);
  statusLabel.setJustificationType(juce::Justification::centredRight);
  for (auto *l : {&nameLabel, &statusLabel}) addAndMakeVisible(*l);
  sizeBox.addItemList({"Size 100%", "Size 85%", "Size 70%"}, 1);
  {
    const float cur = proc.uiScale > 0 ? proc.uiScale : 1.f;
    sizeBox.setSelectedItemIndex(cur > .92f ? 0 : cur > .77f ? 1 : 2, juce::dontSendNotification);
  }
  sizeBox.onChange = [this] {
    const float s[] = {1.f, .85f, .7f};
    const float v = s[juce::jlimit(0, 2, sizeBox.getSelectedItemIndex())];
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<JaySampleView>(this), v] { if (safe && safe->onScale) safe->onScale(v); });
  };
  addAndMakeVisible(sizeBox);

  // waveform + tools
  addAndMakeVisible(wave);
  findButton.setTooltip("Find Samples: puts the 16 strongest hits on this bank's pads (keeps each pad's settings)");
  findButton.onClick = [this] { proc.findSamples(bank(), float(sensSlider.getValue()), false); };
  rerollButton.setTooltip("A different pick of good hits");
  rerollButton.onClick = [this] { proc.findSamples(bank(), float(sensSlider.getValue()), true); };
  sensSlider.setSliderStyle(juce::Slider::LinearBar);
  sensSlider.setRange(0, 1, .01);
  sensSlider.setValue(proc.sensitivity, juce::dontSendNotification);
  sensSlider.setTextValueSuffix(" sens");
  sensSlider.setColour(juce::Slider::trackColourId, accent.withAlpha(.55f));
  sensSlider.setColour(juce::Slider::backgroundColourId, panel2);
  sensSlider.setTooltip("Sensitivity: higher = quieter hits count too");
  sliceBox.addItemList({"4 slices", "8 slices", "16 slices"}, 1);
  sliceBox.setSelectedItemIndex(2, juce::dontSendNotification);
  sliceButton.setTooltip("Even slices across the sample (whole bars when the BPM is known)");
  sliceButton.onClick = [this] { const int n[] = {4, 8, 16}; proc.sliceEvenly(bank(), n[juce::jlimit(0, 2, sliceBox.getSelectedItemIndex())]); };
  snapBox.addItemList({"Snap off", "Snap: zero", "Snap: beat"}, 1);
  snapBox.setSelectedItemIndex(1, juce::dontSendNotification);
  snapBox.onChange = [this] { wave.snapMode = snapBox.getSelectedItemIndex(); };
  zoomIn.onClick = [this] { auto p = proc.project(); if (p && p->sample) { auto &q = p->cues[size_t(selectedCue())]; wave.zoom(.6, q.present ? (q.start + q.end) / 2 : 0); } };
  zoomOut.onClick = [this] { wave.zoom(1.6, 0); };
  zoomFit.onClick = [this] { wave.fit(); };
  waveHint.setText("Drag pad edges  |  double-click: start the selected pad here  |  wheel: zoom", juce::dontSendNotification);
  waveHint.setFont(juce::Font(juce::FontOptions(11.f)));
  waveHint.setColour(juce::Label::textColourId, muted);
  waveHint.setJustificationType(juce::Justification::centredRight);
  for (juce::Component *c : std::initializer_list<juce::Component *>{&findButton, &rerollButton, &sensSlider, &sliceBox, &sliceButton, &snapBox,
                                                                    &zoomOut, &zoomIn, &zoomFit, &waveHint})
    addAndMakeVisible(*c);

  // banks + pads
  for (int b = 0; b < 4; b++) {
    auto &btn = bankButtons[size_t(b)];
    btn.setButtonText("BANK " + juce::String::charToString(juce_wchar('A' + b)));
    btn.setColour(juce::TextButton::buttonOnColourId, accent);
    btn.setClickingTogglesState(false);
    btn.onClick = [this, b] { setParam("bank", float(b)); syncCuePanel(true); pads.repaint(); wave.repaint(); };
    addAndMakeVisible(btn);
  }
  addAndMakeVisible(pads);

  // cue panel
  cueTitle.setFont(juce::Font(juce::FontOptions(14.f)).boldened());
  cueTitle.setColour(juce::Label::textColourId, accent);
  addAndMakeVisible(cueTitle);
  cueName.setTooltip("Pad name");
  cueName.setColour(juce::TextEditor::backgroundColourId, panel2);
  cueName.setColour(juce::TextEditor::outlineColourId, line);
  cueName.onReturnKey = cueName.onFocusLost = [this] {
    const auto n = cueName.getText().toStdString();
    editSelected([n](forge::Cue &q) { q.name = n; });
  };
  addAndMakeVisible(cueName);
  modeBox.addItemList({"Trigger", "Hold", "Loop"}, 1);
  modeBox.setTooltip("Trigger: plays to the end. Hold: plays while held. Loop: loops until you hit it again");
  modeBox.onChange = [this] { if (!syncing) { const int m = modeBox.getSelectedItemIndex(); editSelected([m](forge::Cue &q) { q.mode = m; }); } };
  addAndMakeVisible(modeBox);
  knob(pitchK, pitchL, "PITCH", -24, 24, 1, " st").onValueChange = [this] { if (!syncing) { const float v = float(pitchK.getValue()); editSelected([v](forge::Cue &q) { q.pitch = v; }); } };
  knob(gainK, gainL, "GAIN", 0, 100, 1, " %").onValueChange = [this] { if (!syncing) { const float v = float(gainK.getValue()) / 100; editSelected([v](forge::Cue &q) { q.gain = v; }); } };
  knob(attackK, attackL, "ATTACK", 0, 1000, 1, " ms").onValueChange = [this] { if (!syncing) { const float v = float(attackK.getValue()) / 1000; editSelected([v](forge::Cue &q) { q.attack = v; }); } };
  knob(releaseK, releaseL, "RELEASE", 0, 2000, 1, " ms").onValueChange = [this] { if (!syncing) { const float v = float(releaseK.getValue()) / 1000; editSelected([v](forge::Cue &q) { q.release = v; }); } };
  knob(filterK, filterL, "FILTER", 100, 20000, 1, " Hz").onValueChange = [this] { if (!syncing) { const float v = float(filterK.getValue()); editSelected([v](forge::Cue &q) { q.cutoff = v; }); } };
  filterK.setSkewFactorFromMidPoint(2000);
  attackK.setSkewFactorFromMidPoint(150);
  releaseK.setSkewFactorFromMidPoint(250);
  for (auto *k : {&pitchK, &gainK, &attackK, &releaseK, &filterK}) k->setDoubleClickReturnValue(true, k == &pitchK ? 0 : k == &gainK ? 80 : k == &attackK ? 5 : k == &releaseK ? 40 : 20000);
  reverseT.onClick = [this] { if (!syncing) { const bool v = reverseT.getToggleState(); editSelected([v](forge::Cue &q) { q.reverse = v; }); } };
  chokeT.setTooltip("Hitting this pad again cuts its last hit");
  chokeT.onClick = [this] { if (!syncing) { const bool v = chokeT.getToggleState(); editSelected([v](forge::Cue &q) { q.choke = v; }); } };
  clearCueButton.onClick = [this] { editSelected([](forge::Cue &q) { q = forge::Cue(); }); };
  for (juce::Component *c : std::initializer_list<juce::Component *>{&reverseT, &chokeT, &clearCueButton, &dragTile}) addAndMakeVisible(*c);

  // key + tempo
  auto label = [this](juce::Label &l, const juce::String &t) {
    l.setText(t, juce::dontSendNotification);
    l.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
    l.setColour(juce::Label::textColourId, muted);
    addAndMakeVisible(l);
  };
  label(srcKeyL, "SAMPLE KEY");
  label(srcBpmL, "SAMPLE BPM");
  srcKeyBox.addItem("Unknown", 1);
  for (int k = 0; k < 24; k++) srcKeyBox.addItem(forge::keyName(k), k + 2);
  srcKeyBox.setTooltip("The sample's key (detected; change it if it's wrong)");
  srcKeyBox.onChange = [this] { if (!syncing) { const int k = srcKeyBox.getSelectedId() - 2; proc.editProject([k](ForgeProject &pr) { pr.sourceKey = k; pr.keyDetected = k >= 0; }); } };
  addAndMakeVisible(srcKeyBox);
  srcBpm.setSliderStyle(juce::Slider::IncDecButtons);
  srcBpm.setRange(30, 300, .1);
  srcBpm.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, 24);
  srcBpm.setTooltip("The sample's own tempo (detected; drag or type to fix it)");
  srcBpm.onValueChange = [this] { if (!syncing) { const double b = srcBpm.getValue(); proc.editProject([b](ForgeProject &pr) { pr.sourceBpm = b; pr.bpmDetected = true; }); } };
  addAndMakeVisible(srcBpm);
  halfButton.onClick = [this] { srcBpm.setValue(juce::jlimit(30.0, 300.0, srcBpm.getValue() / 2)); };
  doubleButton.onClick = [this] { srcBpm.setValue(juce::jlimit(30.0, 300.0, srcBpm.getValue() * 2)); };
  addAndMakeVisible(halfButton);
  addAndMakeVisible(doubleButton);
  bar(shiftS, shiftL, "KEY SHIFT").setTooltip("Moves every pad up or down in key, without changing the tempo");
  shiftA = std::make_unique<SA>(proc.parameters, "transpose", shiftS);
  targetKeyL.setFont(juce::Font(juce::FontOptions(15.f)).boldened());
  targetKeyL.setColour(juce::Label::textColourId, accent);
  targetKeyL.setJustificationType(juce::Justification::centred);
  addAndMakeVisible(targetKeyL);
  syncT.setTooltip("On: the sample follows Live's tempo. Off: plays at Target BPM");
  addAndMakeVisible(syncT);
  syncA = std::make_unique<BA>(proc.parameters, "sync", syncT);
  bar(tbpmS, tbpmL, "TARGET BPM");
  tbpmA = std::make_unique<SA>(proc.parameters, "bpm", tbpmS);
  origButton.setTooltip("Original tempo: Target BPM = the sample's BPM, Sync off");
  origButton.onClick = [this] { if (auto cur = proc.project()) { setParam("sync", 0); setParam("bpm", float(cur->sourceBpm)); } };
  addAndMakeVisible(origButton);
  formantsT.setTooltip("Keeps voices natural when shifting key");
  polyT.setTooltip("On: pads layer. Off: one sound at a time");
  keyboardT.setTooltip("Plays the selected pad chromatically across your keyboard (C3 = original pitch)");
  for (auto *t : {&formantsT, &polyT, &keyboardT}) addAndMakeVisible(*t);
  formantsA = std::make_unique<BA>(proc.parameters, "formants", formantsT);
  polyA = std::make_unique<BA>(proc.parameters, "poly", polyT);
  keyboardA = std::make_unique<BA>(proc.parameters, "keyboard", keyboardT);
  label(quantL, "QUANTIZE");
  quantBox.addItemList({"Off", "1/4", "1/8", "1/16"}, 1);
  quantBox.setTooltip("Pad hits land on Live's beat grid while it plays");
  addAndMakeVisible(quantBox);
  quantA = std::make_unique<CA>(proc.parameters, "quantize", quantBox);
  bar(masterS, masterL, "MASTER");
  bar(lowS, lowL, "LOW CUT");
  bar(highS, highL, "HIGH CUT");
  masterA = std::make_unique<SA>(proc.parameters, "master", masterS);
  lowA = std::make_unique<SA>(proc.parameters, "lowcut", lowS);
  highA = std::make_unique<SA>(proc.parameters, "highcut", highS);
  label(padKeysL, "PAD KEYS");
  padRangeL.setJustificationType(juce::Justification::centred);
  padRangeL.setFont(juce::Font(juce::FontOptions(13.f)).boldened());
  padRangeL.setColour(juce::Label::textColourId, accent);
  addAndMakeVisible(padRangeL);
  padDown.onClick = [this] { setParam("padbase", float(proc.padBase() - 12)); };
  padUp.onClick = [this] { setParam("padbase", float(proc.padBase() + 12)); };
  learnButton.setTooltip("Press LEARN, then hit the key or pad you want as pad 1 (pads 2-16 are the next keys up)");
  learnButton.setColour(juce::TextButton::buttonOnColourId, accent);
  learnButton.onClick = [this] { proc.padLearn = !proc.padLearn.load(); };
  followT.setToggleState(true, juce::dontSendNotification);
  followT.setTooltip("Hitting a pad on your controller selects it here");
  for (juce::Component *c : std::initializer_list<juce::Component *>{&padDown, &padUp, &learnButton, &followT}) addAndMakeVisible(*c);

  for (int i = 0; i < 16; i++) seenHits[size_t(i)] = proc.padHits[size_t(i)].load();
  setSize(W, H);
  syncCuePanel(true);
  startTimerHz(30);
}

JaySampleView::~JaySampleView() { stopTimer(); }

void JaySampleView::syncCuePanel(bool force) {
  auto p = proc.project();
  const int ci = selectedCue();
  const uint64_t ver = p ? p->version : 0;
  if (!force && ci == shownCue && ver == shownVersion) return;
  shownCue = ci;
  shownVersion = ver;
  syncing = true;
  const bool has = p && p->cues[size_t(ci)].present;
  const forge::Cue q = p ? p->cues[size_t(ci)] : forge::Cue();
  cueTitle.setText("PAD " + juce::String::charToString(juce_wchar('A' + ci / 16)) + juce::String(ci % 16 + 1) + (has ? "" : "  (empty)"), juce::dontSendNotification);
  if (!cueName.hasKeyboardFocus(true)) cueName.setText(juce::String(q.name), false);
  modeBox.setSelectedItemIndex(q.mode, juce::dontSendNotification);
  pitchK.setValue(q.pitch, juce::dontSendNotification);
  gainK.setValue(q.gain * 100, juce::dontSendNotification);
  attackK.setValue(q.attack * 1000, juce::dontSendNotification);
  releaseK.setValue(q.release * 1000, juce::dontSendNotification);
  filterK.setValue(q.cutoff, juce::dontSendNotification);
  reverseT.setToggleState(q.reverse, juce::dontSendNotification);
  chokeT.setToggleState(q.choke, juce::dontSendNotification);
  for (juce::Component *c : std::initializer_list<juce::Component *>{&cueName, &modeBox, &pitchK, &gainK, &attackK, &releaseK, &filterK, &reverseT, &chokeT, &clearCueButton})
    c->setEnabled(has);
  if (p) {
    srcKeyBox.setSelectedId(p->sourceKey + 2, juce::dontSendNotification);
    srcBpm.setValue(p->sourceBpm, juce::dontSendNotification);
  }
  syncing = false;
}

void JaySampleView::timerCallback() {
  auto p = proc.project();
  nameLabel.setText(p && p->sample ? p->sample->name : juce::String("JaySample"), juce::dontSendNotification);
  statusLabel.setText(proc.status() + "  |  Live " + juce::String(proc.hostBpm.load(), 1) + " BPM", juce::dontSendNotification);
  for (int b = 0; b < 4; b++) bankButtons[size_t(b)].setToggleState(b == bank(), juce::dontSendNotification);
  syncCuePanel(false);
  // pad hits: flash, and Follow selects the pad you hit on your controller
  const double now = juce::Time::getMillisecondCounterHiRes();
  for (int i = 0; i < 16; i++)
    if (const int h = proc.padHits[size_t(i)].load(); h != seenHits[size_t(i)]) { seenHits[size_t(i)] = h; pads.flashUntil[size_t(i)] = now + 150; }
  if (const int hit = proc.lastHitPad.exchange(-1); hit >= 0 && followT.getToggleState() && hit != selectedPad() &&
                                                    !juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
    selectPad(hit);
  // key shift -> the key you hear
  if (p && p->sourceKey >= 0) {
    const int shift = juce::roundToInt(shiftS.getValue());
    const int k = ((p->sourceKey % 12 + shift) % 12 + 12) % 12 + (p->sourceKey >= 12 ? 12 : 0);
    targetKeyL.setText(juce::String(forge::keyName(p->sourceKey)) + "  ->  " + forge::keyName(k), juce::dontSendNotification);
  } else targetKeyL.setText(shiftS.getValue() == 0 ? "original key" : juce::String(juce::roundToInt(shiftS.getValue())) + " st", juce::dontSendNotification);
  tbpmS.setAlpha(syncT.getToggleState() ? .45f : 1.f);
  padRangeL.setText(juce::MidiMessage::getMidiNoteName(proc.padBase(), true, true, 3) + " - " + juce::MidiMessage::getMidiNoteName(proc.padBase() + 15, true, true, 3),
                    juce::dontSendNotification);
  learnButton.setToggleState(proc.padLearn.load(), juce::dontSendNotification);
  learnButton.setButtonText(proc.padLearn.load() ? "HIT A KEY" : "LEARN");
  dragTile.setAlpha(p && p->cues[size_t(selectedCue())].present ? 1.f : .4f);
  pads.repaint();
  wave.repaint();
}

bool JaySampleView::isInterestedInFileDrag(const juce::StringArray &files) {
  for (const auto &f : files) {
    const juce::File file(f);
    if (ForgeProcessor::isAudioFile(file) || file.hasFileExtension("sforge")) return true;
  }
  return false;
}

void JaySampleView::filesDropped(const juce::StringArray &files, int, int) {
  dropHover = false;
  repaint();
  for (const auto &f : files) {
    const juce::File file(f);
    if (ForgeProcessor::isAudioFile(file) || file.hasFileExtension("sforge")) { openFile(file); return; }
  }
}

void JaySampleView::paint(juce::Graphics &g) {
  g.fillAll(bg);
  auto box = [&g](juce::Rectangle<int> r, const juce::String &title) {
    g.setColour(panel);
    g.fillRoundedRectangle(r.toFloat(), 8);
    g.setColour(line);
    g.drawRoundedRectangle(r.toFloat().reduced(.5f), 8, 1);
    if (title.isNotEmpty()) {
      g.setColour(muted);
      g.setFont(juce::Font(juce::FontOptions(11.f)).boldened());
      g.drawText(title, r.reduced(12, 6).removeFromTop(14), juce::Justification::left);
    }
  };
  g.setColour(accent);
  g.setFont(juce::Font(juce::FontOptions(24.f)).boldened());
  g.drawText("JAYSAMPLE", headerR.withWidth(200), juce::Justification::centredLeft);
  box(waveR, "");
  box(padsR, "PADS");
  box(cueR, "");
  box(keyR, "KEY  |  TEMPO  |  OUTPUT");
}

void JaySampleView::paintOverChildren(juce::Graphics &g) {
  if (!dropHover) return;
  g.setColour(bg.withAlpha(.8f));
  g.fillRect(getLocalBounds());
  g.setColour(accent);
  g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(12), 12, 3);
  g.setFont(juce::Font(juce::FontOptions(26.f)).boldened());
  g.drawText("Drop to load", getLocalBounds(), juce::Justification::centred);
}

void JaySampleView::resized() {
  headerR = {16, 8, W - 32, 44};
  {
    auto h = headerR;
    h.removeFromLeft(210);
    loadButton.setBounds(h.removeFromLeft(80).reduced(0, 7));
    h.removeFromLeft(6);
    stopButton.setBounds(h.removeFromLeft(70).reduced(0, 7));
    h.removeFromLeft(12);
    sizeBox.setBounds(h.removeFromRight(110).reduced(0, 8));
    h.removeFromRight(10);
    statusLabel.setBounds(h.removeFromRight(360));
    nameLabel.setBounds(h);
  }
  waveR = {12, 56, W - 24, 258};
  {
    auto r = waveR.reduced(10, 8);
    auto tools = r.removeFromBottom(30);
    r.removeFromBottom(6);
    wave.setBounds(r);
    findButton.setBounds(tools.removeFromLeft(120));
    tools.removeFromLeft(6);
    sensSlider.setBounds(tools.removeFromLeft(130).reduced(0, 3));
    tools.removeFromLeft(6);
    rerollButton.setBounds(tools.removeFromLeft(80));
    tools.removeFromLeft(16);
    sliceBox.setBounds(tools.removeFromLeft(100));
    tools.removeFromLeft(4);
    sliceButton.setBounds(tools.removeFromLeft(60));
    tools.removeFromLeft(16);
    snapBox.setBounds(tools.removeFromLeft(110));
    tools.removeFromLeft(16);
    zoomOut.setBounds(tools.removeFromLeft(30));
    tools.removeFromLeft(4);
    zoomIn.setBounds(tools.removeFromLeft(30));
    tools.removeFromLeft(4);
    zoomFit.setBounds(tools.removeFromLeft(44));
    tools.removeFromLeft(10);
    waveHint.setBounds(tools);
  }
  const int top = waveR.getBottom() + 10, h = H - top - 12;
  padsR = {12, top, 430, h};
  cueR = {padsR.getRight() + 10, top, 320, h};
  keyR = {cueR.getRight() + 10, top, W - 12 - (cueR.getRight() + 10), h};
  {
    auto r = padsR.reduced(10, 8).withTrimmedTop(18);
    auto banks = r.removeFromTop(28);
    const int bw = banks.getWidth() / 4;
    for (int b = 0; b < 4; b++) bankButtons[size_t(b)].setBounds(banks.removeFromLeft(bw).reduced(2, 0));
    r.removeFromTop(8);
    pads.setBounds(r);
  }
  {
    auto r = cueR.reduced(12, 8);
    cueTitle.setBounds(r.removeFromTop(22));
    auto row = r.removeFromTop(26);
    cueName.setBounds(row.removeFromLeft(r.getWidth() - 110));
    row.removeFromLeft(8);
    modeBox.setBounds(row);
    r.removeFromTop(8);
    auto k1 = r.removeFromTop(100);
    r.removeFromTop(8);
    auto k2 = r.removeFromTop(100);
    auto place = [](juce::Slider &k, juce::Label &l, juce::Rectangle<int> a) { l.setBounds(a.removeFromTop(14)); k.setBounds(a.reduced(4, 0)); };
    const int kw = k1.getWidth() / 3;
    place(pitchK, pitchL, k1.removeFromLeft(kw));
    place(gainK, gainL, k1.removeFromLeft(kw));
    place(filterK, filterL, k1);
    place(attackK, attackL, k2.removeFromLeft(kw));
    place(releaseK, releaseL, k2.removeFromLeft(kw));
    {
      auto t = k2.reduced(4, 10);
      reverseT.setBounds(t.removeFromTop(26));
      t.removeFromTop(4);
      chokeT.setBounds(t.removeFromTop(26));
    }
    r.removeFromTop(6);
    clearCueButton.setBounds(r.removeFromTop(28).removeFromLeft(110));
    r.removeFromTop(10);
    dragTile.setBounds(r.removeFromTop(48));
  }
  {
    auto r = keyR.reduced(12, 8).withTrimmedTop(20);
    auto row = [&r](int hgt = 26) { auto x = r.removeFromTop(hgt); r.removeFromTop(6); return x; };
    {
      auto a = row();
      srcKeyL.setBounds(a.removeFromLeft(86));
      srcKeyBox.setBounds(a);
    }
    {
      auto a = row();
      srcBpmL.setBounds(a.removeFromLeft(86));
      doubleButton.setBounds(a.removeFromRight(34));
      a.removeFromRight(4);
      halfButton.setBounds(a.removeFromRight(34));
      a.removeFromRight(6);
      srcBpm.setBounds(a);
    }
    {
      auto a = row();
      shiftL.setBounds(a.removeFromLeft(86));
      shiftS.setBounds(a);
    }
    targetKeyL.setBounds(row(22));
    {
      auto a = row();
      syncT.setBounds(a.removeFromLeft(130));
      origButton.setBounds(a.removeFromRight(56));
    }
    {
      auto a = row();
      tbpmL.setBounds(a.removeFromLeft(86));
      tbpmS.setBounds(a);
    }
    {
      auto a = row();
      quantL.setBounds(a.removeFromLeft(86));
      quantBox.setBounds(a.removeFromLeft(90));
      a.removeFromLeft(10);
      formantsT.setBounds(a);
    }
    {
      auto a = row();
      polyT.setBounds(a.removeFromLeft(90));
      keyboardT.setBounds(a);
    }
    for (auto pr : {std::pair<juce::Label *, juce::Slider *>{&masterL, &masterS}, {&lowL, &lowS}, {&highL, &highS}}) {
      auto a = row(22);
      pr.first->setBounds(a.removeFromLeft(86));
      pr.second->setBounds(a);
    }
    {
      auto a = row();
      padKeysL.setBounds(a.removeFromLeft(70));
      padDown.setBounds(a.removeFromLeft(26));
      learnButton.setBounds(a.removeFromRight(76));
      a.removeFromRight(4);
      padUp.setBounds(a.removeFromRight(26));
      padRangeL.setBounds(a);
    }
    followT.setBounds(row(22).removeFromLeft(120));
  }
}
