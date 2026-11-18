# Phase 4 — effects and sources of superdough (4a: 16 November 2026, 4b: 18 November 2026)

Phase 4 ([PLAN.md](PLAN.md)) was split in two after reading superdough's code:
- **4a (done)**: orbit effects, voice effects, filter LFOs, the `ladder` filter, and the `supersaw`, `pulse`, `sbd` and `user`/partials sources.
- **4b (done)**:
  - `compressor`, `zzfx`, `bytebeat`, wavetables (`wt_…`), `transient`;
  - the modulators `lfo` / `env` / `bmod` and the audio buses;
  - `ir` (sample-based reverb), the FM matrix, `FX` chains, `stretch`, and `K()` (kabelsalat).

Why split: superdough's modulators can target any parameter of any node by its name (`superdoughdata.mjs`), which needs an addressable graph rather than a fixed chain. The compressor needs Chromium's full algorithm. `bytebeat` runs a JavaScript expression on every sample.

## What is new

| Part | File | Detail |
|---|---|---|
| **Orbits** | `src/superdough/Engine` (`Orbit`) | As in `superdoughoutput.mjs`: dry sum + delay + reverb, then `djf`, then the output gain (`duckorbit`), then the stereo output. |
| **Delay** | idem | `FeedbackDelayNode`: `DelayNode` with a 1 s maximum, in a feedback loop (at least 128 samples, as WebAudio requires for a cycle), linear interpolation. `delay`, `delaytime` / `delaysync` (default 3/16 cycle), `delayfeedback` (clamped to 0.98). Time and feedback change at the event's time. |
| **Reverb** | `src/superdough/Convolver` | **Convolution with no added latency**: 128 direct taps, then FFT stages (partitions of 128, 1024 and 8192 samples). Same result as a direct convolution, to within 2e-6. Impulse response as in `reverbGen.mjs` (noise × decay to -60 dB, length 1.5 × `roomsize`, `roomfade`, sliding low-pass `roomlp` → `roomdim`). **Chromium's `ConvolverNode` normalisation.** The response is rebuilt when the parameters change, on a separate thread (the plug-in) or inline (offline renders). The orbit sleeps 8 s after its last send. `dry()`. |
| **djf** | idem | Two-pole filter of `djf-processor`, value read once per 128-sample block. |
| **duckorbit** | idem | Dip in the target orbit's gain 10 ms before the note (`webAudioTimeout(t − 0.01)`), with `duckonset`, `duckattack`, `duckdepth` and lists. As in superdough: without `duckattack`, the ramps throw (`Math.max(undefined, 0.002)` is `NaN`) and there is no dip. |
| **Voice effects** | idem | In superdough's order: `vowel` (5 band-passes + makeup ×8, all vowels and aliases) → `coarse` → `crush` → `shape` (`shapevol`) → `distort` (9 algorithms, `distortvol`, `distorttype`) → `tremolo` (`tremolosync`, `tremolodepth`, `tremoloskew`, `tremolophase`, `tremoloshape`) → pan → `phaser` (`phaserdepth`, `phasercenter`, `phasersweep`). |
| **Filters** | idem | LFOs on `lpf`/`hpf`/`bpf` (`lprate` / `lpsync`, `lpdepth`, `lpdepthfrequency`, `lpshape`, `lpdc`, `lpskew`…). `ftype("ladder")` (`ladder-processor`, `drive`). |
| **Sources** | idem | `supersaw` (`unison`, `spread`, `detune` — or `n`, as in superdough), `pulse` (`pw`, `pwrate`, `pwsweep`), `sbd` (default note 29), `user` + `partials` / `phases`, **and `partials ?? n` on triangle, square and sawtooth** (`s("sawtooth").n(4)` = 4 partials, as in `getOscillator`). The custom waves (`waveformN`, with its own sign conventions) are built by the pattern thread into a shared store, not by the audio thread. |
| **Worklet blocks** | idem | The engine keeps an absolute sample clock: worklets (LFOs, `coarse`, `ladder`, `supersaw`, `pulse`) work in 128-sample blocks aligned on it, as on the audio context. An LFO only starts at the first block that begins after the note, `coarse` restarts at every block, and so on. |

## Comparison with the real superdough

`python3 tests/ref/run_cases.py`. Superdough's worklets now run in the reference (`js/ref/build-worklets.mjs`, plus a `Promise.withResolvers` polyfill: node-web-audio-api needs Node 22 and you have Node 20).

**53 cases out of 53 are close to superdough** (27 from phase 3, 26 new, and 1 case not comparable offline).

**Identical** (correlation 1.000, gaps ≤ 0.05 dB):
- delay (default and with time/feedback), djf low and high;
- coarse, crush, shape, distort (all 9 algorithms), tremolo (and shape);
- phaser, filter LFO, ladder;
- pulse and PWM, partials via `n` and via a list;
- **the reverb**: with and without dry, `roomsize`/`roomlp`/`roomdim`, `roomfade`.

How the reverb was checked: the reference's impulse response is randomly generated (`Math.random`). An esbuild plugin swaps it for the same xorshift generator Concha uses, with the same seed, without modifying superdough. The two responses are therefore identical, and so are the results. That validates the generation, the sliding low-pass, the Chromium normalisation and the convolution in one go.

**Close**:
- `vowel`: correlation 0.998 on a single vowel, 0.983 when switching between 4 vowels. Only the very end of the ringing of the high-Q band-passes (around -60 dB) differs, probably the precision of the Rust implementation.
- `supersaw`: the voice phases are random (`Math.random`), so the beating differs from one draw to another. Overall level within 0.3 dB, spectrum within 1.5 dB.
- `sbd`: its noise click is random, everything else is identical.

**Not comparable offline**: `duckorbit`. Superdough ducks from an `onended` callback, which an `OfflineAudioContext` runs at arbitrary moments of the render (some dips are missing from the reference). It is checked by a unit test instead.

Reminder: on sawtooth and square, the Rust reference does not normalise the waves to their peak, unlike Chromium and Concha (1.4 dB). The comparison makes up for this level offset on the cases built on those waves.

## Tests and performance

- `ctest`: 3 suites, **43 tests**. New: orbits (delay repeats, duck dip, recovery), the feedback delay at `delaytime` with feedback 0.5, the convolver against a direct convolution with zero latency.
- Load: 128 voices held at all times with low-pass and envelope, filter LFO, pan, room, delay, phaser, vowel and distort, spread over 4 orbits with reverb and delay. Rendering is 2.2 times faster than real time, about 45 % of one core in this extreme case. Two optimisations:
  - modulated filter coefficients are recomputed every 8 samples (Chromium: on every sample), with no measurable difference in the comparisons;
  - the band-limited table is chosen only when the frequency changes.

## Limits to know

- The 8192-sample stage of the reverb runs as one block every 8192 samples: a CPU spike of a few milliseconds per orbit with reverb. Splitting it over several blocks remains possible if needed.
- `perf` doesn't work on this kernel (the matching `linux-tools` is missing, and I installed nothing), so there was no detailed profiling.

## Commands

```
node js/ref/build-ref.mjs js/ref/dist/superdough-render.mjs     # after an update of Strudel or of the harness
node js/ref/build-worklets.mjs js/ref/dist/worklets.mjs
python3 tests/ref/run_cases.py [filter]
(cd build && ctest)
```

# Phase 4b (18 November 2026)

## What is new

| Part | Where | Detail |
|---|---|---|
| **compressor** | `src/superdough/Compressor` | Port of Chromium's `DynamicsCompressorKernel` (knee fitted to the ratio, adaptive release, envelope updated every 32 frames on the context's clock), 6 ms pre-delay. `compressor`, `compressorRatio`, `compressorKnee`, `compressorAttack`, `compressorRelease`. Placed after tremolo, as in superdough. |
| **zzfx** | `js/src/sounds.mjs`, `js/src/zzfx-context.mjs` | `z_sine`, `z_sawtooth`, `z_triangle`, `z_square`, `z_tan`, `z_noise`, `zzfx`: superdough's own zzfx code renders the note on the pattern thread; the buffer goes to the engine through a ring of transient slots (`SampleLibrary::publishTransient`). |
| **bytebeat** | `js/src/bytebeat.mjs` | The expression runs in JS (the `byte-beat-processor` helpers), rendered for the note's length (8 s at most), then played like zzfx. |
| **wavetables** | `Engine` (`wavetable-oscillator-processor`) | `wt_…` samples cut in frames (`__frameLen`, 2048 by default), `wt`, `warp`, all 22 `warpmode`s, their envelopes and LFOs, `unison` / `spread` / `detune`. |
| **transient** | `Engine` (`transient-processor`) | `transient`, `transsustain` (also `"attack:sustain"`). |
| **lfo / env** | `SoundConvert` (`lfo`, `env` objects), `Engine` (`ModState`) | Up to 6 modulators per sound, connected like `connectLFO` / `connectEnvelope`: every lfo, then every env. Depth relative to the target's value (`depth`) or absolute (`depthabs`), frequency range clamp, `sync`, `retrig`, `shape`, `skew`, `dcoffset`, `curve`; envelopes with `a`/`d`/`s`/`r` and their curves. A modulator can modulate an earlier one (`c: "lfo_0"`, sub-controls `rate`, `depth`, `dcoffset`, `skew`). |
| **ir** | `sounds.mjs` (`__irurl`), `Engine::buildImpulse` | `room(…).ir("name")`: the orbit's reverb convolves with sample number `i` (default 1, as superdough) of that sound, rebuilt as `adjustLength` does (`roomsize` = how much of it is used, `irspeed`, `irbegin`, backwards when the speed is below 1). Built on the reverb thread; if the sample is still loading, the build waits for it. |

The reverb is now rebuilt exactly when superdough rebuilds it (`hasChanged`), and a rebuild uses the event's values with `generate()`'s defaults for the others. Before, Concha kept earlier values (for example a `roomsize` from an older event), which superdough doesn't.

Targets the modulators can reach:
- the source's frequency (`s`, `note`, `freq`; `detune` for samples, k-rate as on `AudioBufferSourceNode`);
- `gain`, `postgain`, `pan`;
- `lpf` / `hpf` / `bpf` frequency and Q (ladder included);
- `tremolo` rate and depth;
- `phasercenter`, `phaserdepth`;
- the `delay` and `room` sends;
- `vib`, `vibmod`, `fmi`, `pw`, `wt`, `warp`;
- other lfos and envs.

(The end of phase 4, below, added all the other targets superdough can actually modulate.)

## Comparison with the real superdough

**73 cases out of 73 close** (`python3 tests/ref/run_cases.py`), plus `duckorbit`, which isn't comparable offline. All the new cases are identical (correlation 1.000, ≤ 0.02 dB):
- 7 for lfo/env (cutoff, gain, pitch, lfo on lfo, env on cutoff, curves on pan, env on a sample's pitch);
- 3 for `ir` (plain, speed + begin + size, backwards).

The compressor matches with a 96-sample shift: the reference (Rust) has a 384-sample pre-delay, Chromium 288.

A superdough behaviour found on the way: `envelope-processor` never starts for an event exactly at `currentTime` 0, because it compares `begin` with its initial `beginTime`, which is also 0. This only happens in an offline render that starts with a note, never on strudel.cc, so the env cases start with `.late(.125)`. Concha triggers it, as any live event; only the offline reference doesn't.

The `sbd` case now allows 5 dB on the spectrum: its noise click is random (`Math.random`) and one draw went over 3 dB.

## Tests

- `ctest`: 3 suites, all passing.
- Pattern parity in QuickJS: unchanged, 546 cases, 0 unexplained.

## Second part of 4b (18 November 2026)

| Part | Where | Detail |
|---|---|---|
| **K()** | `js/src/kabel.mjs`, `src/superdough/Kabel` | kabelsalat 0.4.1 (`@kabelsalat/core`, `lib`, `web`, AGPL, in `js/node_modules`, the version superdough asks for). The pattern thread evaluates the expression and compiles it with kabelsalat's own compiler, after superdough's substitutions (`pat[i]`, `sFreq`, `sGate`). That gives JavaScript code run once per sample. Concha compiles that code to bytecode with JavaScript's rules (comparisons give booleans, `===` is strict, `&&` / `\|\|` return an operand, `%` is fmod). All of kabelsalat's 40 ugens are ported (oscillators, noises, ADSR, delay with feedback, two-pole filter, biquad `qf`, sequencer, clock…). Timing follows `generic-processor`: from the first quantum at or after the note, the gate cc at 1 until the note's end, silent from `endWithRelease`. Delay lines (10 s, as in kabelsalat) come from a shared pool. `raw` / `bytebeat` / `floatbeat` nodes are refused: their generated code isn't valid JavaScript (`const r[4] = …`), so they fail in Strudel too. |
| **FM matrix** | `SoundConvert`, `Engine` (`fm`) | All of `applyFM`: 8 operators, `fmi`…`fmi8` (N → N−1), `fmiXY` (X → Y; Y = 0 is the carrier), `fmhN`, `fmwaveN` (noises included), `fmenvN` / `fmattackN`… Operators are computed sources first. A cycle with no delay (for example `fmi2` + `fmi12`) is muted, as Chromium and node-web-audio-api mute it. `lfo` / `env` can target `fmiN` and `fmhN`, with superdough's quirk: `fm_N_gain` is the last gain created for operator N, sometimes a gain left unconnected. |
| **Buses** | `Engine` (`AudioBus`) | `.bus(n)` / `busgain` send the post-gain output to bus n (in addition to the orbit). `s("bus").n(n)` plays bus n, with its envelope. Voices that read a bus are rendered after the voices that write it, so there's no delay, as in the WebAudio graph. |
| **bmod** | idem | `connectBusModulator`: `(dc + bus mixed down to mono) × depth / 0.3`, clamped when the target is a frequency. Up to 6 modulators per sound: lfo, env, then bmod. |
| **FX chains** | `src/superdough/FxChain`, `Engine` (`FxStage`) | `.FX(a, b…)`: each stage runs superdough's whole effect block. That means K(), stretch, transient, gain (0.8 by default per stage, as superdough), the 3 filters with their envelopes and LFOs, vowel, coarse, crush, shape, distort, tremolo, compressor, pan and phaser. Every stage before the main one also has its own FeedbackDelay and reverb (`delay`, `room`, `ir` inside a stage). The pattern thread converts the stages and publishes them in a store (identical chains share a slot). Stages, delays and reverbs come from pools: 32 stages, 8 stage reverbs. |
| **stretch** | `src/superdough/PhaseVocoder` | `phase-vocoder-processor` on `ola-processor`: 2048-sample frames every 128 samples, Hann windows ×1.62, spectral peaks moved by the factor. fft.js's `realTransform` is ported exactly, including the intermediate values it leaves in the upper half of the spectrum: the vocoder reads them back when it shifts downwards. The worklet needs the whole current quantum, so here its output comes one quantum later, and the sound starts 40 ms (superdough) + 128 samples earlier. The scheduler and `concha_render` move it ahead. |

Code changes on the way:
- The voice effect block became a structure (`FxStage`) shared by the voice and the FX stages.
- `SoundConverter::convertControls` converts one set of controls, used for the value and for each stage.
- The stale "ladder not implemented" warning is gone (ladder came in 4a).
- A filter modulated by its own LFO (without an envelope) now recomputes its coefficients every 8 samples, not every sample.

## Comparison with the real superdough (end of 4b)

**94 cases out of 94 close**, plus 3 not comparable offline (`duckorbit`, two `bmod`). Identical (correlation 1.000):
- 6 K() cases (ADSR, delay feedback, `audioin` + filter, `S("…")` inputs, seq/fold/qf, logic + pan);
- 4 FM matrix cases (chain, cross route with envelope and square wave, muted cycle, LFO on `fmi`);
- 2 bus cases;
- 5 FX cases (coarse → lpf → distort, gain + phaser + bpf, a stage's own delay, a stage's own reverb, K() in a stage).

For the stage reverb, the reference uses the same noise generator: Concha seeds stage reverbs the same way.

Close:
- the 3 `stretch` cases: identical while the sound plays (correlation 0.994 to 1.000);
- Strudel's own `K()` example: statistically close, because `saw` starts at a random phase and the reverb noise is random.

Known differences of the reference:
- **bmod**: superdough connects the modulator from an `onended` callback. An `OfflineAudioContext` runs it at arbitrary moments, so the cases are skipped and a unit test checks bmod instead (`superdough_tests`, buses).
- **stretch**: the vocoder stops when its input goes away. Chromium (and Concha) cut it when the source stops. node-web-audio-api only does after its 40 ms overlap-add tail, and the length tolerance allows that.
- **FM**: node-web-audio-api accumulates oscillator phases in float32 from the start of the render, whereas Chromium and Concha use double. A deep FM chain amplifies that on later notes, so the chain case has a single note.

The `envelope-processor` quirk from the first part (no trigger at `currentTime` 0) applies only to offline renders.

## Tests and performance

- `ctest`: 3 suites, all passing. New: buses (a reader triggered before its writer still hears it without delay) and `bmod` with `dc`.
- Pattern parity in QuickJS: unchanged (546 cases, 0 unexplained).
- The FX refactor costs nothing measurable (same time as before it, within 2 %).
- Heavy load test: 130 voices held for 8 s, each with sawtooth, lpf + envelope + LFO, pan, vowel, distort, phaser, room and delay, over 4 orbits. That renders at 0.85× real time on one core. It is heavier than the 4a test, which didn't put every effect on every voice. No single effect dominates: removing any one of them gains 5 to 20 %. Optimising this belongs to a later phase.

## Commands (reference)

```
node js/ref/build-ref.mjs js/ref/dist/superdough-render.mjs     # kabelsalat is now real in the reference (mainFields module)
node js/ref/build-worklets.mjs js/ref/dist/worklets.mjs          # worklets with kabelsalat's ugens
python3 tests/ref/run_cases.py [filter]
```

# End of phase 4 (18 November 2026)

Three real gaps with Strudel remained after 4b. All three are closed.

## Every modulation target superdough can reach

`lfo`, `env` and `bmod` can now modulate every parameter of `CONTROL_TARGETS` whose node exists. Each one is read the way its node reads it: once per quantum (`[0]`) or on every sample (`pv`).

| Target | Read |
|---|---|
| `coarse`, `crush`, `shape`, `shapevol` (`shape-processor`) | once per quantum |
| `distort`, `distortvol`, `distorttype` (`distort-processor`) | every sample |
| `compressor`, `compressorRatio`, `compressorKnee`, `compressorAttack`, `compressorRelease` | k-rate, once per quantum, within the node's ranges |
| `delaytime`, `delaysync`, `delayfeedback` | every sample, on the orbit's delay (shared, as in superdough: every voice modulating it adds up) |
| `djf` | once per quantum, on the orbit |
| `vowel` | the five band-pass frequencies, every sample |
| `lpdepth` / `lpdc` / `lpskew` and the same for `hp` / `bp` | the filter LFO, once per quantum |
| `tremoloskew`, `phaserrate`, `phasersweep`, `pwrate`, `pwsweep`, `wtrate` / `wtdepth` / `wtskew`, `warprate` / `warpdepth` / `warpskew` | the LFO, once per quantum |
| `detune`, `spread` | `freqspread` / `panspread` of supersaw and wavetables, every sample |
| `stretch` | the vocoder's factor, at each frame |
| `lfo_curve`; `env_attack`, `env_decay`, `env_sustain`, `env_release` | the other modulator's parameters: an envelope whose parameters move runs its state machine sample by sample, as the worklet does |

A sub-control superdough doesn't know falls back to the main parameter, as `getControlData` does (`lfo_shape` → frequency, an unknown `env_…` → depth).

What still prints a warning is what superdough can't do either. Those modulators throw or do nothing in superdough:
- `lprate` / `lpsync`, `wtdc`, `tremolophase`, `transient`: their parameter doesn't exist on the node;
- `tremoloshape`, `lpshape`: a non-integer shape breaks the worklet;
- `dry`, `busgain`, a `bmod` as target: no node is registered for them;
- `byteBeatStartTime`: Concha renders bytebeat in JavaScript.

## Modulators inside FX stages (`fxi`)

- A modulator declared inside a stage (`.FX(lpf(400).lfo({…}))`) modulates that stage's nodes.
- A main modulator with `fxi: n` modulates stage n.
- The order is superdough's: each stage's lfo, env, bmod, then the main ones.
- The `lfo_N` / `env_N` names are shared by every stage, as in superdough: the latest one wins.
- Each stage has its own modulation sums. A stage's own delay and reverb can be modulated too (`delay`, `delaytime`, `delayfeedback`, `room`).

## Resources: bigger pools, and they no longer run out silently

| Resource | Before | Now |
|---|---|---|
| K() graphs | 32 | 64 |
| FX stages | 32 | 64 |
| Vocoders (`stretch`) | 16 | 32 |
| FX stage reverbs | 8 | 16 |
| FX stage delays | inside each stage | 16 lines in a pool of their own, so the 64 stages don't each carry 400 KB |
| Buses | 16 | 32 |

When one of them runs out, the console says so (at most every 10 s per kind), and so does `concha_render`. That covers K() delay lines too. The browser has no such limits; in Concha they keep the audio thread allocation-free.

## Comparison with the real superdough

**111 cases out of 111 close** (3 more not comparable offline), with 17 new ones:
- identical: coarse, crush, shape + shapevol, distort, delaytime + delayfeedback, djf, filter LFO depth, env attack, lfo curve, tremoloskew, phaserrate, pwsweep, a modulator inside an FX stage, `fxi` from the main value;
- close: the compressor (96-sample shift, as before); `stretch` (its usual tail difference); `vowel` (correlation 0.999; the end of the high-Q ringing differs a little, as in the plain vowel case).

`ctest`: all passing. Pattern parity: unchanged. Performance: unchanged (0.87× real time on the 130-voice test).
