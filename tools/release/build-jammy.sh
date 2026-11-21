#!/bin/sh
# Release build for Ubuntu 22.04 (glibc 2.35), run inside an Ubuntu 22.04 root filesystem with gcc-12,
# CMake >= 3.25, ninja and the -dev packages of libcurl, freetype, fontconfig, sndfile, X11, Xinerama,
# ALSA and GL. Here: MidiPad's devtools/jammy.sh with the "Plugin Development" folder mounted at /src:
#
#   ninja -C build                     # on the host first: makes build/js/{concha-strudel.js,docs.json}
#   SRC="$PWD/.." ../MidiPad/devtools/jammy.sh sh /src/Concha/tools/release/build-jammy.sh
#
# Result: build/jammy/Concha_artefacts/Release/{CLAP,VST3} (the standalone is not built).
set -e
cd /src/Concha
export CONCHA_PREBUILT_JS=/src/Concha/build/js CC=gcc-12 CXX=g++-12 # juceaide is configured on its own
cmake -S . -B build/jammy -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc-12 -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_AR=/usr/bin/gcc-ar-12 -DCMAKE_RANLIB=/usr/bin/gcc-ranlib-12 \
  -DCONCHA_NODE=/src/Concha/tools/release/prebuilt-node.sh -DCONCHA_BUILD_SPIKES=OFF -DCONCHA_JACK=OFF
ninja -C build/jammy Concha_CLAP Concha_VST3
