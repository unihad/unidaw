cmake_minimum_required(VERSION 3.22)

project(unidaw VERSION 0.1.0 LANGUAGES C CXX)

set(TE_ADD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Eigene Engine-/JUCE-Korrekturen (bis upstream), siehe patches/apply.sh
execute_process(COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.sh RESULT_VARIABLE dawd_patch_result)
if(NOT dawd_patch_result EQUAL 0)
    message(FATAL_ERROR "dawd/patches/apply.sh failed")
endif()
# Neue Patch-Fassung: neues Configure, damit apply.sh sie anwendet
file(GLOB dawd_patches CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/*.patch)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply.sh ${dawd_patches})

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tracktion_engine tracktion_build)

juce_add_console_app(unidaw NEEDS_CURL FALSE NEEDS_WEB_BROWSER FALSE)
target_compile_definitions(unidaw PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_ALSA=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1 JUCE_USE_MP3AUDIOFORMAT=1)
# Ressourcen findet unidaw relativ zur Binary (source/Resources.h), nicht ueber
# Pfade in den Quellordner. Im Build-Ordner liegen dafuer Links neben der
# Binary — UI-Aenderungen wirken weiter mit einem Browser-Reload.
foreach(dawd_res ui kits models plugin-profiles ../recipes launcher/unidaw-start)
    get_filename_component(dawd_res_name "${dawd_res}" NAME)
    add_custom_command(TARGET unidaw POST_BUILD COMMAND ${CMAKE_COMMAND} -E create_symlink
                       "${CMAKE_CURRENT_SOURCE_DIR}/${dawd_res}" "$<TARGET_FILE_DIR:unidaw>/${dawd_res_name}" VERBATIM)
endforeach()
target_compile_features(unidaw PRIVATE cxx_std_20)
# Op about (Lizenz, Quellcode, Drittlizenzen; AGPLv3 § 13): Version aus
# project(), Link aufs öffentliche Repo (Standard = homepage in
# mcp/manifest.json; ein Fork setzt -DDAWD_SOURCE_URL=…, leer = kein Link),
# THIRD_PARTY_LICENSES.md der Repo-Wurzel als Bytes ins Binary — das
# AppImage braucht dafür keinen Quellordner. Ändert sich die Datei, läuft
# das Configure neu; der Header wird nur bei neuem Inhalt geschrieben.
set(UNIDAW_SOURCE_URL "https://github.com/unihad/unidaw" CACHE STRING "Public source repository shown in the UI (About / Licenses)")
set(dawd_third_party_file ${CMAKE_SOURCE_DIR}/../THIRD_PARTY_LICENSES.md)
set(dawd_third_party_bytes "")
if(EXISTS ${dawd_third_party_file})
    file(READ ${dawd_third_party_file} dawd_third_party_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," dawd_third_party_bytes "${dawd_third_party_hex}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${dawd_third_party_file})
else()
    # Datei kommt später dazu: der Ordner ändert sich, das Configure läuft neu
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/..)
endif()
file(WRITE ${CMAKE_BINARY_DIR}/generated/DawdAbout.h.tmp
     "// generiert von CMakeLists.txt (Op about)\n#pragma once\n"
     "#define UNIDAW_VERSION \"${PROJECT_VERSION}\"\n"
     "#define UNIDAW_SOURCE_URL \"${UNIDAW_SOURCE_URL}\"\n"
     "static const unsigned char dawdThirdPartyLicenses[] = { ${dawd_third_party_bytes} 0x00 };\n")
configure_file(${CMAKE_BINARY_DIR}/generated/DawdAbout.h.tmp ${CMAKE_BINARY_DIR}/generated/DawdAbout.h COPYONLY)
target_include_directories(unidaw PRIVATE ${CMAKE_BINARY_DIR}/generated)
# JACK-Backend (fuer pipewire-jack, UNIDAW_AUDIO=jack): nur mit Headern
# (Paket libjack-jackd2-dev); libjack laedt JUCE erst zur Laufzeit.
include(CheckIncludeFile)
check_include_file(jack/jack.h UNIDAW_HAVE_JACK)
if(UNIDAW_HAVE_JACK)
    target_compile_definitions(unidaw PRIVATE JUCE_JACK=1 JUCE_JACK_CLIENT_NAME="unidaw")
endif()
target_sources(unidaw PRIVATE
    source/main.cpp
    source/Resources.cpp
    source/Dawd.cpp
    source/EngineHelpers.cpp
    source/Routing.cpp
    source/Feedback.cpp
    source/Sidechain.cpp
    source/Automation.cpp
    source/Undo.cpp
    source/MusicAnalysis.cpp
    source/SampleLibrary.cpp
    source/Origins.cpp
    source/SoundIndex.cpp
    source/SoundFeatures.cpp
    source/Clap.cpp
    source/Transcription.cpp
    source/Separation.cpp
    source/GuidedSplit.cpp
    source/PluginSettings.cpp
    source/PluginRandomness.cpp
    source/Limiter.cpp
    source/Presets.cpp
    source/PluginProfiles.cpp
    source/Interface.cpp
    source/PluginWindows.cpp
    source/XErrors.cpp
    source/Osc.cpp
    source/PluginSandbox.cpp
    source/PluginHost.cpp
    source/RecordSession.cpp
    source/Capture.cpp
    source/OpRegistry.cpp
    source/ops/Session.cpp
    source/ops/Tree.cpp
    source/ops/Audio.cpp
    source/ops/Compact.cpp
    source/ops/Sections.cpp
    source/ops/Media.cpp
    source/ops/Select.cpp
    source/ops/Processing.cpp
    source/ops/Freeze.cpp
    source/ops/ClipFx.cpp
    source/ops/Groups.cpp
    source/ops/Comp.cpp
    source/ops/Analysis.cpp
    source/ops/AnalysisSource.cpp
    source/ops/Voice.cpp
    source/ops/Drums.cpp
    source/DrumAnalysis.cpp
    source/VoiceAnalysis.cpp
    source/ops/Library.cpp
    source/ops/Presets.cpp
    source/ops/SoundMatch.cpp
    source/ops/Recipes.cpp
    source/ops/Stems.cpp
    source/ops/Automation.cpp
    source/ops/View.cpp
    source/ops/Interface.cpp
    source/ops/Capture.cpp
    source/ops/Osc.cpp)
target_link_libraries(unidaw PRIVATE tracktion::tracktion_engine)
target_link_libraries(unidaw PRIVATE "-latomic")
# Audio-Interface-Regler (Interface.cpp) ueber alsa-lib
target_link_libraries(unidaw PRIVATE asound)

# ML-Ops (audioToMidi, separateStems): ONNX Runtime liegt nicht im Repo —
# tools/fetch_ml.sh laedt sie nach third_party/; ohne bleiben die Ops aus
set(UNIDAW_ORT_DIR "${CMAKE_SOURCE_DIR}/third_party/onnxruntime-linux-x64-1.31.0")
if (EXISTS "${UNIDAW_ORT_DIR}/lib/libonnxruntime.so")
    message(STATUS "unidaw: ONNX Runtime found — ML ops enabled")
    target_include_directories(unidaw PRIVATE "${UNIDAW_ORT_DIR}/include")
    target_link_libraries(unidaw PRIVATE "${UNIDAW_ORT_DIR}/lib/libonnxruntime.so")
    target_compile_definitions(unidaw PRIVATE UNIDAW_WITH_ONNX=1)
    set_target_properties(unidaw PROPERTIES BUILD_RPATH "${UNIDAW_ORT_DIR}/lib")
else()
    message(STATUS "unidaw: ONNX Runtime missing — run tools/fetch_ml.sh for the ML ops")
endif()

# Timestretch/Pitch fuer Audio-Clips: Rubberband (GPL) als Single-File-Build
# der Engine, wenn tools/fetch_ml.sh die Quellen geholt hat
if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/rubberband/single/RubberBandSingle.cpp")
    message(STATUS "unidaw: Rubberband found — time-stretch enabled")
    target_include_directories(unidaw PRIVATE "${CMAKE_SOURCE_DIR}/third_party")
    target_compile_definitions(unidaw PRIVATE TRACKTION_ENABLE_TIMESTRETCH_RUBBERBAND=1 TRACKTION_BUILD_RUBBERBAND=1 UNIDAW_WITH_RUBBERBAND=1)
else()
    message(STATUS "unidaw: Rubberband missing — run tools/fetch_ml.sh for time-stretch")
endif()
