#pragma once

// STK's wave tables (sc3-plugins' external_libraries/stk/rawwaves), compiled into Concha
// (cmake/Sc3Plugins.cmake) and written once to Concha's cache folder, where STK reads them:
// RAWWAVE_PATH is concha_stk_rawwave_path() for the StkUGens and StkInst units.

// The folder, with a trailing separator (STK appends the file name).
const char* concha_stk_rawwave_path();
// Writes the files that are missing or changed. Not on the audio thread (concha_sc_new).
void concha_stk_extract_rawwaves();
