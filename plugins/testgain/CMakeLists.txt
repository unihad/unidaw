cmake_minimum_required(VERSION 3.22)

project(testgain VERSION 0.1.0 LANGUAGES C CXX)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tracktion_engine/modules/juce juce_build)

juce_add_plugin(testgain
    PLUGIN_MANUFACTURER_CODE Unid
    PLUGIN_CODE Tg01
    FORMATS VST3 LV2
    LV2URI "https://unihad.com/plugins/testgain"
    COMPANY_NAME "unihad"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(testgain PRIVATE TestGain.cpp TestGain.h)
target_compile_definitions(testgain PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(testgain PRIVATE juce::juce_audio_utils)
