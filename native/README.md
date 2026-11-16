# JaySample 0.4.0: Serato Sample-style sampler (AU / VST3)

Everything happens inside the plugin, so you don't need the browser. Built for **macOS High Sierra 10.13+ and Ableton Live 10**, universal (Intel + Apple Silicon).

## Workflow
1. **Load:** drop a sample on the window, or press **LOAD**. WAV, AIFF, MP3, FLAC, M4A, OGG and CAF work, up to 10 minutes. Old browser `.sforge` projects open too.
2. **Analysis:** JaySample detects the **BPM** and **key**, and **Find Samples** puts the 16 strongest hits on bank A. You can change all of it.
3. **Play:** hit pads **C1–D#2** (MIDI 36–51) on your controller, or click them. PAD KEYS (< > / LEARN) moves the pads anywhere on your keyboard.
4. **Edit:**
   - drag a pad's edges on the waveform
   - double-click to start the selected pad at that spot
   - mouse wheel to zoom; drag to scroll; click the overview strip to jump
5. **Save:** everything saves inside your Live set, including the sample, so there's nothing to keep track of.

## Features
**Waveform**
- Overview strip plus a zoomable main view.
- Pads of the current bank show as coloured regions, with a live playhead.
- Snap to zero crossings (no clicks) or to the beat grid (1/16 of the sample's BPM).

**Find Samples / Re-roll / Slice**
- **Find Samples** picks the strongest hits. **Sensitivity** sets how quiet a hit can be and still count.
- **Re-roll** gives a different set.
- **Slice** cuts even 4 / 8 / 16 slices across whole bars.
- Pad settings are kept when you re-find.

**Pads:** 4×4 grid with pad 1 at the bottom-left, 4 banks (A–D), 64 pads in all.
- **Lighting:** a pad lights while it plays and flashes when hit.
- **Follow:** hitting a pad on your controller selects it for editing.

**Per pad**
- Name, and Trigger / Hold / Loop (loop pads toggle).
- Pitch (±24 st), Gain, Attack, Release, Filter.
- Reverse.
- **Choke**: hitting it again cuts its last hit.

**Key and tempo**
- Sample key and sample BPM (detected, editable, ½ / ×2).
- **Key Shift** ±12 st, keeping tempo. It shows the key you'll hear (e.g. Am → Cm).
- **Sync to Live** (stretch to Live's tempo), or **Target BPM**. **ORIG** sets the original tempo.
- **Formants** keeps voices natural when shifting.

**Playback**
- **Quantize** (1/4, 1/8, 1/16) puts pad hits on Live's beat grid.
- **Poly / mono**: up to 32 voices.
- **Keyboard mode**: play the selected pad chromatically, with C3 at the original pitch.

**Output:** Master, Low Cut, High Cut, and a safety limiter that only acts on peaks above −1 dBFS.

**Drag out:** drag the tile in the pad panel onto a Live track to get that pad as a WAV, at the current key and tempo. The files are kept in `~/Music/JaySample Exports`.

**Click-free:** a 2 ms fade-in on every hit, and 8–10 ms fade-outs for Stop, choke, mono, voice stealing (the oldest voice goes first) and key or tempo changes.

## Build (Codemagic, the same setup as GuitarForge)
**Codemagic:**
1. Push this `native` folder's contents to your GitHub repo root (`codemagic.yaml`, `CMakeLists.txt`, `Source/`, `ThirdParty/`, `tests/`).
2. Run the **jaysample-universal** workflow.

Its steps: DSP tests → build AU + VST3 (Xcode 16.4, JUCE 8.0.10, 10.13 target) → ad-hoc sign → **check that the binary is set for High Sierra** → zip → auval.

**On the High Sierra Mac:** after installing the AU, run `auval -v aumu Sf03 SmFg`.

**Local build:** `scripts/build-mac.command` (add `--install` to copy it into your plugin folders).

## Tests
- **`tests/dsp_test.cpp`** (no JUCE, runs first in CI):
  - pitch and tempo stay independent
  - declick on start and stop
  - limiter, low cut and high cut
  - hits found within 20 ms and never late
  - Find Samples picks the strongest hits, in order
  - Re-roll gives different sets
  - BPM (92 reads as 92), key (Am progression reads as A minor)
  - slicing and snapping
- **`tests/host_test.cpp`** drives the real plugin with no DAW (`-DJAYSAMPLE_HOST_TEST=ON`):
  - loads a WAV and detects the BPM; Find Samples gives 1 pad per hit
  - MIDI plays pad 1 under −1 dBFS; lights and playhead work
  - Stop fades out; quantize lands on the beat (within 64 samples)
  - choke, key shift and PAD KEYS work
  - Live-set save and restore is bit-exact; WAV export works
  - the window opens

  Both suites passed on Linux with JUCE 8.0.10. The Mac build and Live 10 still need checking on your machine.

## Not in this version
Stems (AI separation) are planned for a later build. There are no per-pad effects yet. The editor works in the plugin only; the browser version in `../dist` still works and its `.sforge` files open here.

## Licences
- **JaySample source:** MIT.
- **Signalsmith Stretch / Linear:** MIT (see `ThirdParty/`).
- **JUCE 8:** AGPLv3 or commercial; it's fetched at build time and isn't bundled.
