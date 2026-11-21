#!/bin/sh
# Stands in for node in builds without it (Ubuntu 22.04 release build, tools/release/build-jammy.sh):
# `node <script> ... <output>` copies $CONCHA_PREBUILT_JS/<output's name>, made by a normal build
# (build/js/concha-strudel.js, build/js/docs.json).
for last; do :; done
exec cp "${CONCHA_PREBUILT_JS:?set CONCHA_PREBUILT_JS}/$(basename "$last")" "$last"
