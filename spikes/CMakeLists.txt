# Phase 0 experiments. Not part of the plug-in.

# QuickJS-ng comes from cmake/StrudelBundle.cmake.
add_executable(spike_qjs_tunes qjs_tunes.cpp)
target_link_libraries(spike_qjs_tunes PRIVATE qjs)

# Embedded scsynth: two Worlds in one process.
include(${PROJECT_SOURCE_DIR}/cmake/ScSynth.cmake)
add_executable(spike_sc_two_worlds sc/two_worlds.cpp)
target_include_directories(spike_sc_two_worlds PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(spike_sc_two_worlds PRIVATE concha_scsynth)
