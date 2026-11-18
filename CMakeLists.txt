cmake_minimum_required(VERSION 3.25)

# ------------------------------------------------------------------------------------------
# Concha: Strudel (codeberg.org/uzu/strudel) as a CLAP / VST3 plug-in. AGPL-3.0-or-later.
# Plan: ~/.claude/plans (phase 0 = skeleton + risk spikes).
# ------------------------------------------------------------------------------------------

set(CONCHA_SHARED_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/../deps" CACHE PATH
    "Shared third-party folder (JUCE, clap-juce-extensions, SuperCollider sources, dr_libs, stb)")
set(CONCHA_JUCE_DIR "${CONCHA_SHARED_DEPS}/JUCE-8.0.15" CACHE PATH "JUCE 8 checkout")
set(CONCHA_CLAP_JUCE_DIR "${CONCHA_SHARED_DEPS}/clap-juce-extensions" CACHE PATH "clap-juce-extensions checkout")

project(Concha VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(CONCHA_BUILD_SPIKES "Build the phase 0 experiments in spikes/" ON)

add_subdirectory(${CONCHA_JUCE_DIR} ${PROJECT_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
add_subdirectory(${CONCHA_CLAP_JUCE_DIR} ${PROJECT_BINARY_DIR}/clap-juce-extensions EXCLUDE_FROM_ALL)

include(cmake/StrudelBundle.cmake)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CONCHA_CURL REQUIRED IMPORTED_TARGET libcurl)

# superdough in C++ (JUCE-free, real-time safe).
add_library(concha_superdough STATIC src/superdough/Nodes.cpp src/superdough/Engine.cpp src/superdough/Convolver.cpp
    src/superdough/Compressor.cpp
    src/superdough/Kabel.cpp
    src/superdough/FxChain.cpp
    src/superdough/PhaseVocoder.cpp)
target_link_libraries(concha_superdough PUBLIC Threads::Threads)
target_include_directories(concha_superdough PUBLIC src)

# Sample library: named folders, decoding (dr_libs, stb_vorbis from ../deps), http cache (libcurl).
add_library(concha_library STATIC src/library/Decoder.cpp src/library/SampleLibrary.cpp)
target_include_directories(concha_library PUBLIC src PRIVATE ${CONCHA_SHARED_DEPS}/dr_libs ${CONCHA_SHARED_DEPS}/stb)
target_link_libraries(concha_library PUBLIC concha_superdough PkgConfig::CONCHA_CURL Threads::Threads)
set_source_files_properties(src/library/Decoder.cpp PROPERTIES COMPILE_OPTIONS "-w")

# Scheduler: pattern thread (QuickJS) -> audio thread. JUCE-free.
add_library(concha_sched STATIC src/sched/PatternWorker.cpp src/sched/AudioScheduler.cpp src/sched/SoundConvert.cpp)
target_include_directories(concha_sched PUBLIC src)
target_link_libraries(concha_sched PUBLIC concha_pattern concha_library Threads::Threads)

add_executable(concha_query tools/concha_query/concha_query.cpp)
target_link_libraries(concha_query PRIVATE concha_pattern concha_library)

add_executable(concha_render tools/concha_render/concha_render.cpp)
target_link_libraries(concha_render PRIVATE concha_sched)

juce_add_plugin(Concha
    COMPANY_NAME "Zencha"
    COMPANY_WEBSITE "https://zencha.org"
    PLUGIN_MANUFACTURER_CODE Zcha
    PLUGIN_CODE Cnch
    PRODUCT_NAME "Concha"
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    NEEDS_CURL TRUE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth Generator)

clap_juce_extensions_plugin(TARGET Concha
    CLAP_ID "org.zencha.concha"
    CLAP_FEATURES instrument synthesizer note-effect sampler)

# Strudel's function reference for the editor (help, completion), from the JSDoc of its packages
set(CONCHA_DOCS ${PROJECT_BINARY_DIR}/js/docs.json)
add_custom_command(
    OUTPUT ${CONCHA_DOCS}
    COMMAND ${CONCHA_NODE} ${PROJECT_SOURCE_DIR}/js/tools/gen-docs.mjs ${PROJECT_SOURCE_DIR}/third_party/strudel/packages ${CONCHA_DOCS}
    DEPENDS ${concha_js_inputs} ${PROJECT_SOURCE_DIR}/js/tools/gen-docs.mjs
    COMMENT "Extracting Strudel's function reference"
    VERBATIM)
add_custom_command(
    OUTPUT ${PROJECT_BINARY_DIR}/js/EmbeddedDocs.cpp
    COMMAND concha_embed ${CONCHA_DOCS} ${PROJECT_BINARY_DIR}/js/EmbeddedDocs.cpp concha embeddedDocs
    DEPENDS concha_embed ${CONCHA_DOCS}
    COMMENT "Embedding the function reference"
    VERBATIM)

target_sources(Concha PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/LibraryPanel.cpp
    src/plugin/SyncProbe.cpp
    src/plugin/ui/Theme.cpp
    src/plugin/ui/JsTokeniser.cpp
    src/plugin/ui/DocIndex.cpp
    src/plugin/ui/CodeView.cpp
    src/plugin/ui/Panels.cpp
    ${PROJECT_BINARY_DIR}/js/EmbeddedDocs.cpp)

target_include_directories(Concha PRIVATE src)

target_compile_definitions(Concha PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    # Standalone: PipeWire answers through both its ALSA and its JACK interfaces.
    JUCE_ALSA=1
    JUCE_JACK=1)

target_link_libraries(Concha
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_opengl
        concha_sched
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Off-screen snapshot of the editor with a pattern playing (UI checks without a DAW).
add_executable(concha_ui_shot tools/ui_shot/ui_shot.cpp)
target_link_libraries(concha_ui_shot PRIVATE Concha)
target_include_directories(concha_ui_shot PRIVATE src $<TARGET_PROPERTY:Concha,INCLUDE_DIRECTORIES>)
target_compile_definitions(concha_ui_shot PRIVATE $<TARGET_PROPERTY:Concha,COMPILE_DEFINITIONS>)

option(CONCHA_BUILD_TESTS "Build the unit tests" ON)
if(CONCHA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(CONCHA_BUILD_SPIKES AND EXISTS "${PROJECT_SOURCE_DIR}/spikes/CMakeLists.txt")
    add_subdirectory(spikes)
endif()
