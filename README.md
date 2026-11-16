# JaySample v0.4

**New in 0.4:** the AU/VST3 plugin in `native/` is now a full Serato Sample-style sampler. You load a sample, it detects the key and BPM, Find Samples sets up the pads, and you edit on the waveform, all inside the plugin. It builds for High Sierra 10.13+ through Codemagic. See `native/README.md`.

A private, browser-based sampling instrument that combines a waveform chopping editor with four performance banks. Native Mac AU/VST3 playback-bridge source is in `native/`.

## Browser features

- Load mono/stereo audio, estimate/correct key and BPM, transpose independently of tempo, optional formant compensation.
- Manual/transient/even cue slicing; zoom to 200×, beat/zero-crossing snap, drag trims and cue names.
- Four banks, 64 cues, trigger/hold/loop, mono/poly playback, velocity-aware MIDI, keyboard pitching, 32 voice limit.
- Spectral pitch/time processing in a dedicated WASM worker, 96 MB prepared-audio cache, bank/keyboard preparation, short loop edge fades, monitor gain/compression and level meter.
- Internal quantization/metronome, incoming MIDI clock, tap tempo. Web MIDI needs a compatible browser and permission; hardware has not been tested here.
- Lossless portable `.sforge` files embedding PCM and all banks; local IndexedDB recovery, 60-step undo/redo.
- Rendered cue WAVs and bank ZIPs with WAVs, MIDI audition sequence, note map and Logic/Ableton instructions.

## Run and test

The app is buildless. Serve `dist/` over HTTP, for example `python3 -m http.server 8000 --directory dist`, then open localhost:8000. Deploy over HTTPS for MIDI support. No audio leaves the browser.

`npm test` runs project/analysis unit tests using Node's test runner. For browser integration checks: `npm install`, `npx playwright install chromium`, then `npm run test:browser`. You may set `PLAYWRIGHT_CHROMIUM_PATH` to an existing Chromium executable. Integration tests run an ephemeral local server on 8766 and store test assets in ignored `work/`.

The native source has its own README and Mac build script. No Mac plugin binaries are supplied or claimed to have been host-validated.

## Practical limits

Audio import: 128 MB, stereo/mono, maximum 10 minutes. Project audio is lossless float PCM, so project files may be larger than the original compressed source. Cached audio is capped at 96 MB; bank exports at 256 MB. Prepare transformed cues before performing. Key/BPM detection is a heuristic estimate and remains editable; synthetic fixture results do not establish accuracy on commercial recordings. Extreme pitch/time settings may introduce artifacts. No stems or Serato proprietary engine is included.

Recovery is local to this browser and origin, and may be cleared by browser storage settings. Download `.sforge` files for permanent backups.

## Dependencies

Signalsmith Stretch/Linear and JS bindings (MIT), fflate (MIT); licenses are included with the vendored assets. Native JUCE is fetched at a pinned hash under its applicable AGPL/commercial license. JaySample native source is MIT.
