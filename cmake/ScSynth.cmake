# scsynth (SuperCollider server, GPL-3.0-or-later) compiled inside Concha, with the "host" audio
# driver of src/sc/SC_HostDriver.cpp: the plug-in hands it its blocks, no JACK or PortAudio.
# Source: ../deps/supercollider-<version> (unmodified checkout, tag Version-<version>).

set(CONCHA_SC_DIR "${CONCHA_SHARED_DEPS}/supercollider-3.13.0" CACHE PATH "SuperCollider source checkout")
set(SC "${CONCHA_SC_DIR}")
set(SCS "${SC}/server/scsynth")

if(NOT EXISTS "${SCS}/SC_World.cpp")
    message(FATAL_ERROR "SuperCollider sources not found at ${SC}")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CONCHA_SNDFILE REQUIRED IMPORTED_TARGET sndfile)
find_package(Threads REQUIRED)

# SC_Version.hpp, as SuperCollider's own CMakeLists.txt makes it.
include("${SC}/SCVersion.txt")
set(PROJECT_VERSION_MAJOR ${SC_VERSION_MAJOR})
set(PROJECT_VERSION_MINOR ${SC_VERSION_MINOR})
set(PROJECT_VERSION_PATCH ${SC_VERSION_PATCH})
set(GIT_COMMIT_HASH "Version-${SC_VERSION}")
set(GIT_BRANCH "concha")
configure_file("${SC}/common/SC_Version.hpp.in" "${PROJECT_BINARY_DIR}/sc/common/SC_Version.hpp")

aux_source_directory("${SC}/external_libraries/boost/libs/system/src" concha_boost_system_src)
aux_source_directory("${SC}/external_libraries/boost/libs/filesystem/src" concha_boost_filesystem_src)
add_library(concha_sc_boost STATIC ${concha_boost_system_src} ${concha_boost_filesystem_src})
target_include_directories(concha_sc_boost SYSTEM PUBLIC "${SC}/external_libraries/boost")
target_compile_definitions(concha_sc_boost PUBLIC BOOST_ALL_NO_LIB)

add_library(concha_sc_tlsf STATIC "${SC}/external_libraries/TLSF-2.4.6/src/tlsf.c")
target_compile_definitions(concha_sc_tlsf PRIVATE TLSF_STATISTIC=1)
target_include_directories(concha_sc_tlsf PUBLIC "${SC}/external_libraries/TLSF-2.4.6/src")

set(concha_scsynth_sources
    ${SCS}/SC_BufGen.cpp ${SCS}/SC_ComPort.cpp ${SCS}/SC_CoreAudio.cpp ${SCS}/SC_Graph.cpp
    ${SCS}/SC_GraphDef.cpp ${SCS}/SC_Group.cpp ${SCS}/SC_Lib_Cintf.cpp ${SCS}/SC_Lib.cpp
    ${SCS}/SC_MiscCmds.cpp ${SCS}/SC_Node.cpp ${SCS}/SC_Rate.cpp ${SCS}/SC_SequencedCommand.cpp
    ${SCS}/SC_Str4.cpp ${SCS}/SC_Unit.cpp ${SCS}/SC_UnitDef.cpp ${SCS}/SC_World.cpp ${SCS}/Rendezvous.cpp
    ${SC}/common/SC_Filesystem_unix.cpp ${SC}/common/SC_fftlib.cpp ${SC}/common/fftlib.c
    ${SC}/common/SC_AllocPool.cpp ${SC}/common/SC_Errors.cpp ${SC}/common/SC_Reply.cpp
    ${SC}/common/SC_StringBuffer.cpp ${SC}/common/SC_StringParser.cpp ${SC}/common/Samp.cpp
    ${SC}/common/sc_popen.cpp ${SC}/common/SC_ServerBootDelayWarning.cpp
    ${PROJECT_SOURCE_DIR}/src/sc/SC_HostDriver.cpp)
if(APPLE)
    list(APPEND concha_scsynth_sources ${SC}/common/SC_Filesystem_macos.cpp ${SC}/common/SC_Apple.mm)
elseif(WIN32)
    list(APPEND concha_scsynth_sources ${SC}/common/SC_Filesystem_win.cpp ${SC}/common/SC_Win32Utils.cpp)
endif()

add_library(concha_scsynth STATIC ${concha_scsynth_sources})
set_target_properties(concha_scsynth PROPERTIES CXX_STANDARD 17 CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden)
target_include_directories(concha_scsynth
    PUBLIC
        "${SC}/include/common" "${SC}/include/server" "${SC}/include/plugin_interface" "${SCS}" "${SC}/common"
        "${PROJECT_BINARY_DIR}/sc/common" "${PROJECT_SOURCE_DIR}/src"
    PRIVATE
        "${SC}/external_libraries/boost_sync/include" "${SC}/external_libraries" "${SC}/external_libraries/nova-tt")
target_compile_definitions(concha_scsynth
    PUBLIC SC_MEMORY_ALIGNMENT=32 SC_AUDIO_API=7 SC_FFT_GREEN
    PRIVATE "SC_PLUGIN_DIR=\"/usr/lib/SuperCollider/plugins\"" "SC_PLUGIN_EXT=\".so\"")
target_compile_options(concha_scsynth PRIVATE -w)
target_link_libraries(concha_scsynth PUBLIC concha_sc_boost concha_sc_tlsf PkgConfig::CONCHA_SNDFILE Threads::Threads ${CMAKE_DL_LIBS})

# ---- UGens, compiled in (STATIC_PLUGINS): no dependency on an installed SuperCollider ----
# SuperCollider's own plug-ins, and the sc3-plugins families SuperDirt's SynthDefs use.
# Each plug-in's PluginLoad(Name) becomes Name_Load(); scsynth calls the core ones from
# initialize_library(), concha_sc_new() calls the others (src/sc/SC_HostDriver.cpp).
set(CONCHA_SC3_DIR "${CONCHA_SHARED_DEPS}/sc3-plugins-3.13.0" CACHE PATH "sc3-plugins source checkout")
set(SCP "${SC}/server/plugins")
set(SC3P "${CONCHA_SC3_DIR}/source")
if(NOT EXISTS "${SC3P}/MdaUGens/MdaUGens.cpp")
    message(FATAL_ERROR "sc3-plugins sources not found at ${CONCHA_SC3_DIR}")
endif()
add_library(concha_sc_ugens STATIC
    ${SCP}/BinaryOpUGens.cpp ${SCP}/ChaosUGens.cpp ${SCP}/DelayUGens.cpp ${SCP}/DemandUGens.cpp ${SCP}/DemoUGens.cpp
    ${SCP}/DynNoiseUGens.cpp ${SCP}/FilterUGens.cpp ${SCP}/GendynUGens.cpp ${SCP}/GrainUGens.cpp ${SCP}/IOUGens.cpp
    ${SCP}/LFUGens.cpp ${SCP}/MulAddUGens.cpp ${SCP}/NoiseUGens.cpp ${SCP}/OscUGens.cpp ${SCP}/PanUGens.cpp
    ${SCP}/PhysicalModelingUGens.cpp ${SCP}/ReverbUGens.cpp ${SCP}/TestUGens.cpp ${SCP}/TriggerUGens.cpp
    ${SCP}/UnaryOpUGens.cpp ${SCP}/UnpackFFTUGens.cpp ${SCP}/DiskIO_UGens.cpp
    ${SCP}/FFTInterfaceTable.cpp ${SCP}/FFT_UGens.cpp ${SCP}/PV_UGens.cpp ${SCP}/PartitionedConvolution.cpp
    ${SCP}/Convolution.cpp ${SCP}/FFT2InterfaceTable.cpp ${SCP}/FeatureDetection.cpp ${SCP}/PV_ThirdParty.cpp
    ${SCP}/ML.cpp ${SCP}/Loudness.cpp ${SCP}/BeatTrack.cpp ${SCP}/Onsets.cpp ${SCP}/onsetsds.c ${SCP}/KeyTrack.cpp
    ${SCP}/MFCC.cpp ${SCP}/BeatTrack2.cpp ${SCP}/ML_SpecStats.cpp
    ${SC3P}/MCLDUGens/MCLDDistortionUGens.cpp
    ${SC3P}/MembraneUGens/Membrane.cpp ${SC3P}/MembraneUGens/Membrane_shape.c
    ${SC3P}/MdaUGens/MdaUGens.cpp
    ${SC3P}/SkUGens/FM7.cpp
    ${SC3P}/AYUGens/AY_UGen.cpp ${SC3P}/AYUGens/AY_libayemu/src/ay8912.c
    ${SC3P}/DistortionUGens/DistortionUGens.cpp
    ${SC3P}/BlackrainUGens/BlackrainUGens.cpp)
set_target_properties(concha_sc_ugens PROPERTIES CXX_STANDARD 17 CXX_VISIBILITY_PRESET hidden C_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(concha_sc_ugens PRIVATE STATIC_PLUGINS SC_MEMORY_ALIGNMENT=32 NOVA_SIMD)
target_include_directories(concha_sc_ugens PRIVATE
    "${SC}/include/common" "${SC}/include/plugin_interface" "${SC}/include/server" "${SC}/common" "${SCP}"
    "${SC}/external_libraries/nova-simd" "${SC}/external_libraries/boost" "${SC}/external_libraries"
    "${SC3P}/AYUGens/AY_libayemu/include" "${SC3P}/MdaUGens" "${SC3P}/MembraneUGens")
target_compile_options(concha_sc_ugens PRIVATE -w -fno-finite-math-only)
target_link_libraries(concha_sc_ugens PRIVATE PkgConfig::CONCHA_SNDFILE)
target_compile_definitions(concha_scsynth PRIVATE STATIC_PLUGINS)
target_link_libraries(concha_scsynth PUBLIC concha_sc_ugens)
