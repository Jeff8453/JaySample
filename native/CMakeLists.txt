cmake_minimum_required(VERSION 3.22)

# Jeff's rig: macOS High Sierra 10.13 (Intel) + Ableton Live 10. Universal, so it also runs on Apple Silicon later.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Oldest macOS" FORCE)
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Universal Mac" FORCE)

project(JaySample VERSION 0.4.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.10          # same JUCE as GuitarForge (builds for 10.13 with Xcode 16.4)
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

set(FORGE_FORMATS VST3)
if(APPLE)
  list(APPEND FORGE_FORMATS AU)
endif()

juce_add_plugin(JaySample
  COMPANY_NAME "Jeff"
  BUNDLE_ID "audio.jaysample.instrument"
  PLUGIN_MANUFACTURER_CODE SmFg
  PLUGIN_CODE Sf03
  FORMATS ${FORGE_FORMATS}
  PRODUCT_NAME "JaySample"
  IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  VST3_CATEGORIES Instrument Sampler
  AU_MAIN_TYPE kAudioUnitType_MusicDevice
  COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(JaySample)
target_sources(JaySample PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp)
target_include_directories(JaySample PRIVATE Source ThirdParty)
target_compile_definitions(JaySample PUBLIC
  JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
if(UNIX AND NOT APPLE)   # Linux compile checks only (no audio devices / extra X11 libraries needed)
  target_compile_definitions(JaySample PUBLIC JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0)
endif()
target_link_libraries(JaySample PRIVATE juce::juce_audio_utils
                      PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# DSP tests (no JUCE): run before every build
add_executable(ForgeDSPTests tests/dsp_test.cpp)
target_include_directories(ForgeDSPTests PRIVATE Source ThirdParty)
target_compile_options(ForgeDSPTests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-UNDEBUG>)
enable_testing()
add_test(NAME NativeDSP COMMAND ForgeDSPTests)

# Optional: drive the real plugin without a DAW (load, play, quantize, save/restore, export, screenshot)
option(JAYSAMPLE_HOST_TEST "Build the plugin host test" OFF)
if(JAYSAMPLE_HOST_TEST)
  juce_add_console_app(JaySampleHostTest PRODUCT_NAME "JaySampleHostTest")
  target_sources(JaySampleHostTest PRIVATE tests/host_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
  target_include_directories(JaySampleHostTest PRIVATE Source ThirdParty)
  juce_generate_juce_header(JaySampleHostTest)
  target_compile_definitions(JaySampleHostTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
  target_link_libraries(JaySampleHostTest PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
