# Concha

Strudel (codeberg.org/uzu/strudel) as a CLAP / VST3 plug-in, Linux first, then Windows and macOS (keep code
portable). AGPL-3.0-or-later. The user writes in French; answer in French.

Plan: `docs/PLAN.md` (approved). Results: `docs/PHASE0.md` (spikes, Bitwig checklist), `docs/PHASE1.md` (pattern engine,
parity with Strudel's snapshots, performance), `docs/PHASE2.md` (scheduler, sync, MIDI out, Bitwig checklist), `docs/PHASE3.md` (superdough C++ v1, sample library), `docs/PHASE4.md` (4a: effects, orbits, sources;
4b: compressor, zzfx, bytebeat, wavetables, transient, lfo/env/bmod on every target, buses, ir, FM matrix, FX chains with
fxi, stretch, K(); phase 4 is complete), `docs/PHASE5.md` (the editor: strudel.cc-like REPL, English UI).

## Rules from the user

- Ask before installing or downloading anything. Shareable libraries go in `../deps/`, project-only ones in
  `third_party/` or `js/node_modules`. No apt packages.
- No default sample folders: the user adds and names every sample folder.
- Audio and MIDI go through the DAW (no JACK routing). The standalone should prefer PipeWire.
- Hosts to validate: Bitwig Studio (CLAP preferred) and Carla.
- Commit only when asked.

## Architecture (see docs/PLAN.md)

- Strudel JS, unmodified (`third_party/strudel`, pinned), bundled by `js/build.mjs` (esbuild; `@strudel/*` resolved
  to `third_party/strudel/packages/*`) and run in QuickJS-ng (`third_party/quickjs-ng`) on a pattern thread.
- Sync: `cps = BPM / 60 / N`, `cycle = ppq / N` (parameter "Beats per cycle", default 4); silent when the DAW stops
  unless "Free run".
- 16 stereo outputs: orbit k -> stereo pair k (orbits 0 and 1 -> Main, 2 -> "Orbit 2" ... 16), as superdough's
  multichannel orbits; `.channels()` overrides.
- DSP: superdough (packages/superdough) rewritten in C++ (`src/superdough`): WebAudio nodes reimplemented to the spec,
  its 17 worklets ported line by line, rendered against superdough for parity. Strudel's `dough` C engine is only a
  source of reusable parts (oscillators, voices, PCM, chorus, flanger) and a possible `.dough()` mode. scsynth embedded (`cmake/ScSynth.cmake`, driver `src/sc/SC_HostDriver.cpp`, C API `src/sc/ConchaSc.h`).

## Where things are

- `js/src/concha.mjs` Strudel runtime (uses Strudel's own `repl()`), `js/src/stubs.mjs` stand-ins for browser-only
  packages (names from `js/src/generated/browser-api.json`, regenerate with `js/tools/gen-browser-api.mjs`),
  `js/src/shims.mjs`. Bundled by `cmake/StrudelBundle.cmake` and compiled into the binary (`tools/embed`).
- `src/pattern/PatternEngine` (QuickJS: evaluate, query -> PatternEvent, timeouts), `tools/concha_query`.
- `js/src/midi.mjs`: Concha's `.midi()` (rules of @strudel/midi; raw bytes out as `midimsg` events).
- `src/sched`: `PatternWorker` (pattern thread, two JS engines alternating: active queries, standby evaluates,
  swap on success), `AudioScheduler` (audio thread: ppq placement, loop laps, jumps = epochs, code swaps,
  note-offs first; RT-safe), `SpscRing`/`SeqLock`, `SchedTypes.h`. While stopped, the start is prepared
  (events for epoch + 1).
- `js/src/kabel.mjs` + `src/superdough/Kabel`: K() (kabelsalat 0.4.1 in js/node_modules compiles; C++ runs the generated
  code as bytecode, ugens ported). `src/superdough/FxChain` (FX stages store), `PhaseVocoder` (stretch).
- `js/src/sounds.mjs`: sound registry (samples(), banks, aliases) and query-time resolution (__type, __url,
  __transpose, __freq added to the value).
- `src/superdough`: `AudioParam` (WebAudio automation), `Nodes` (Chromium-like band-limited oscillators, custom
  PeriodicWaves + `WaveStore`, spec biquads, panner, noises), `Convolver` (zero-latency partitioned convolution,
  Chromium normalisation, reverbGen IRs), `Engine` (voices built like superdough(), orbits with delay / reverb /
  djf / duck; worklet-like stages in 128-sample quanta on an absolute clock), `SoundParams` (NaN = undefined),
  `SampleStore`. `src/sched/SoundConvert` maps hap values to SoundParams and logs unsupported controls.
- `src/library`: `SampleLibrary` (process-wide: named folders in ~/.config/Concha/library.txt, http cache in
  ~/.cache/Concha/http, loader threads), `Decoder` (dr_libs, stb_vorbis), `LibraryHost` (JS natives).
- `src/plugin` (JUCE processor, `PluginEditor` = window: top bar, code, tabs; `LibraryPanel` = sample folders,
  phase 0 SyncProbe). `src/plugin/ui`: `CodeView` (CodeEditorComponent + playing-hap outlines, flash, sliders,
  completion), `JsTokeniser`, `Theme` (strudel.cc colours, flat tabs), `DocIndex` (reference from
  `js/tools/gen-docs.mjs`, embedded), `Panels` (console, reference, sounds, settings, meters). UI prefs live in
  the processor's ValueTree (saved with the project). `tools/ui_shot`: off-screen PNG of the window, playing., `src/sc` (embedded scsynth). `tools/concha_render` renders code to a WAV.
- Parity suite: `js/test/make-cases.mjs`, `js/test/check.mjs`, `js/test/known-differences.json` (each entry is a
  difference caused by Strudel's test runtime, not by Concha; keep "unexplained" at 0).

## Build

```
npm --prefix js ci                      # once: esbuild + Strudel's npm dependencies
cmake -S . -B build -G Ninja && ninja -C build
(cd build && ctest)
```
Artefacts: `build/Concha_artefacts/Release/{CLAP,VST3,Standalone}`, `build/concha_query`. Installed copies used by
Bitwig: `~/.clap/Concha.clap`, `~/.vst3/Concha.vst3` (copy after a build when the user wants to test). Spikes:
`spikes/`, `js/spike/` (commands in docs/PHASE0.md).

## Gotchas

- SynthDefs must use `OffsetOut` to start on the exact sample.
- Do not load the system `UIUGens.so` in the plug-in (its mouse thread aborts at exit).
- Shallow git submodules sometimes arrive empty: check, then `git submodule update --init`.
- Carla 2.5.9 here cannot load CLAP; test CLAP in Bitwig.
- Run parity cases in a fresh JS runtime each: some examples change global settings (setDefaultJoin, useRNG).
- Reference renders: real superdough in node (js/ref, node-web-audio-api; worklets bundled by js/ref/build-worklets.mjs;
  reverbGen's Math.random replaced by Concha's xorshift so IRs match) vs concha_render, `python3 tests/ref/run_cases.py`.
  Not comparable offline: duckorbit (onended callbacks). Random sources (noise, supersaw phases) compared statistically.
  node-web-audio-api (Rust) does not normalise square/sawtooth by their peak; Chromium and Concha do (1.4 dB): known.
- vitest .snap files store `\n` unescaped: check.mjs re-escapes line breaks in expected haps.
- QuickJS evaluation can be slow (caverave: 1.7 s): never evaluate on a thread that must answer queries in time.
- Scheduler tests: wait for the swap (status.activeCode) and run a stopped phase before playing (prepareStart),
  as a DAW does; otherwise the first events are legitimately late.
- In Strudel code, double quotes are mini-notation: use single quotes for samples('...') file names / URLs.
- Tests that play s("bd") must register sounds first (samples({...})): unknown sounds are skipped, like superdough.
- Test sample folders with XDG_CONFIG_HOME / XDG_CACHE_HOME in the scratchpad: never write the user's library.txt.
- superdough's envelope-processor never triggers for an event at currentTime 0 (offline renders): reference cases
  with env start with .late(...).
- Modulators (lfo/env/bmod) target only earlier ones; Engine computes them last-to-first each sample.
- Voice effects live in `Engine::FxStage` (Voice inherits it; FX stages come from a pool): add stage effects there,
  in setupStage / processStage.
- Resource pools (K(), FX stages, vocoders, stage reverbs/delays, buses) report shortages through
  Engine::takeWarnings (console, concha_render).
- Voices reading a bus (s("bus"), bmod) render after the writers; stretched sounds start 40 ms + 128 samples early.
- Reference harness: bmod is not comparable offline (onended callback); @kabelsalat/core must resolve to its module
  build (mainFields).
- UI tests and concha_ui_shot link the plug-in's shared-code target (Concha) and need an X display.
- PatternWorker::activeCodeInfo() (mini locations, sliders, sound names) is read only when the generation changes.
- Pattern threads rely on the 8 MB Linux default stack (QuickJS limit 6 MB): give them an explicit stack on Windows.
