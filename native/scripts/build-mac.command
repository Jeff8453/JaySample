#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [ "$(uname -s)" != Darwin ]; then
  echo 'This AU/VST3 build requires macOS with Xcode command-line tools.'
  exit 1
fi
if ! xcrun --find clang++ >/dev/null 2>&1; then
  echo 'Install Xcode command-line tools with: xcode-select --install'
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  echo 'Install CMake from cmake.org or run: brew install cmake'
  exit 1
fi
cmake -S . -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
printf '\nBuilt universal Mac plugins in build/JaySample_artefacts/Release/\n'
if [ "${1:-}" = '--install' ]; then
  mkdir -p "$HOME/Library/Audio/Plug-Ins/Components" "$HOME/Library/Audio/Plug-Ins/VST3"
  ditto 'build/JaySample_artefacts/Release/AU/JaySample.component' "$HOME/Library/Audio/Plug-Ins/Components/JaySample.component"
  ditto 'build/JaySample_artefacts/Release/VST3/JaySample.vst3' "$HOME/Library/Audio/Plug-Ins/VST3/JaySample.vst3"
  codesign --force --deep --sign - "$HOME/Library/Audio/Plug-Ins/Components/JaySample.component"
  codesign --force --deep --sign - "$HOME/Library/Audio/Plug-Ins/VST3/JaySample.vst3"
  echo 'Installed to your user plugin folders. Restart or rescan Logic/Ableton.'
fi
