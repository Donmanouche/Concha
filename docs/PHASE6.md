# Phase 6 — SuperCollider inside Concha (18 November 2026)

Your rule for this phase: **superdough and SuperDirt are one synth**. You never pick an engine and
never write `.osc()`. Everything strudel.cc does sounds as on strudel.cc (superdough). What only
SuperDirt has is added on top, through the same voice.

The SuperDirt embedded is **StrudelDirt**, the fork you use (notes scaled like Strudel, shimmer,
Juno chorus, Strudel-style gains). It is copied into `third_party/strudeldirt`, so the build does not
depend on your Dropbox.

## How it works

| Part | Where | Detail |
|---|---|---|
| **UGens** | `cmake/ScSynth.cmake` | SuperCollider 3.13's own UGens and the sc3-plugins families StrudelDirt needs (MdaPiano, FM7, Membrane, AY, WaveLoss/Squiz/SawDPW, CrossoverDistortion, BMoog, JoshPV) are **compiled into Concha** (`STATIC_PLUGINS`): nothing comes from an installed SuperCollider. sc3-plugins 3.13.0 is in `../deps` (downloaded with your agreement). Sources stay unmodified. FFT plug-ins made of several files get their own globals, and JoshPV is compiled through `src/sc/JoshPVUGens_safe.cpp`, which zeroes its units' memory: upstream `PV_Freeze` freed garbage pointers when stopped before its first FFT frame. `MouseX`/`MouseY` hold their minimum (a plug-in has no mouse to follow). |
| **SynthDefs** | `tools/sc/compile_synthdefs.sh`, `resources/strudeldirt/synthdefs`, `src/sc/ScDefs` | StrudelDirt's 130 SynthDefs, plus Concha's two copy synths, are written once with the installed `sclang` and its own class library (not yours). They are committed as `.scsyndef` files with an index of their controls, then packed into the binary. All of them load and play in the embedded scsynth (`spike_sc_check_defs`). |
| **Instruments** | `src/sc/ScHost`, `Engine` (`SoundType::ScSynth`) | A sound superdough doesn't have but StrudelDirt does (`superpiano`, `superfm`, `superhex`, `supergong`, `superkick`… **64 instruments**) is played by a scsynth World and becomes the **source of a superdough voice**. That voice gets every superdough effect, orbit and output, like a sample would. The synth gets its controls as StrudelDirt's `DirtEvent` builds them: <ul><li>`freq` from `note` (octave 3 for note names, as `.osc()`) or `(n ?? 36) + octave × 12`;</li><li>`sustain = delta × clip`, `release` (default 0.01), `totalDuration`;</li><li>`speed`, `endSpeed` (accelerate), `cps`, `cycle`, `z1 = n`;</li><li>every number of the hap whose name is one of its controls (`velocity`, `detune`, `voice`, `pitch1`…).</li></ul>`pan` stays centred in the synth, because superdough pans the voice. |
| **Envelope** | `Engine::scEnvelope` | As in StrudelDirt, every note gets its `envelope` module (`strudel_envelope`, always on since `release` defaults to 0.01): an ADSR with `attack` (at least 1 ms), `decay`, `hold` (the level; 1 when there is no `decay`), `release` and `curve` (−2), its gate open for `sustain`, times `amp`. Then `dirt_gate`: no fade-in, a 1 ms sine fade-out at `totalDuration`. |
| **Effects** | `src/sc/ScFx`, `Engine` (`FxDrain`) | The effects superdough doesn't have run **at the end of the voice**, in StrudelDirt's module order, in a second World: `squiz`, `waveloss`, `fshift` (`fshiftnote`, `fshiftphase`), `triode`, `krush` (`kcutoff`), `octer` (`octersub`, `octersubsub`), `ring` (`ringf`, `ringdf`), `distort2`, `psrate`/`psdisp`, `tilt`/`plat`/`curve`, `tremolorate`/`tremolodepth`, and the spectral ones (`tsdelay`, `xsdelay`, `freeze`, `comb`, `smear`, `scram`, `binshift`, `hbrick`, `lbrick`, `real`, `imag`, `enhance`). The voice's sound, after its gain, goes into the World; the modules' output comes back 64 samples later to the voice's orbit, with its `dry`, `delay`, `room` and `bus` sends. A module parameter the hap doesn't have is sent as 0, as StrudelDirt's `nil` is. |
| **Orbit effects** | `src/sc/ScOrbitFx`, `Engine` | StrudelDirt's global effects, in a third World, on each orbit's dry sum: **`leslie`** (`lrate`, `lsize`) and **`shimmer`** (with the orbit's `delaytime` and `delayfeedback`, defaults 0.1875 and 0.15). As `GlobalDirtEffect`, a synth starts on an orbit the first time one of its controls is set, and then gets only the controls that change. `leslie` replaces `leslie × dry` with its own sound, which comes back 64 samples (1.3 ms) later; `shimmer`'s delay time is shortened by those 64 samples, so it lands on time. |
| **Controls** | `js/src/concha.mjs` | StrudelDirt's controls that Strudel doesn't have are pattern functions in Concha: `shimmer`, `psrate`, `psdisp`, `tilt`, `plat`, `tremolorate`, `distort2`, and every control of the SynthDefs played with `s()` (a SynthDef of yours with a `harm` control gives `.harm(2)` and `harm("1 2")`). |
| **Your SynthDefs** | `src/sc/ScDefs`, `LibraryPanel` | **"SynthDef folders"**, in the panel of the sample folders (empty by default, saved in `~/.config/Concha/synthdefs.txt`). Each `.scsyndef` file in a folder (sub-folders included), for example from `SynthDef(\mysynth, {...}).writeDefFile(path)` in SuperCollider, plays with `s("mysynth")` like StrudelDirt's instruments: same voice, same controls rules, every superdough and StrudelDirt effect. Files are read again at **each evaluation** when they changed (date or size), so a SynthDef rewritten in SuperCollider sounds new as soon as you evaluate. The console tells you "loaded n SynthDefs from …" and names the files it cannot read. As StrudelDirt's, a SynthDef should write to `out` with `Out.ar` and free itself (`doneAction: 2`); Concha also stops it after `totalDuration`. |
| **Sample-exact** | `src/sc/SC_HostDriver.cpp` | The audio thread queues its OSC messages in a preallocated queue: `World_SendPacket` allocates. They run at their sample in scsynth's block of 64. StrudelDirt's instruments write with `Out` (block start), so the voice reads their output from that block's start: the note starts at its sample, as `dirt_gate`'s `OffsetOut` does in SuperDirt. A voice with StrudelDirt effects starts 64 samples earlier (the scheduler moves it), so its sound arrives on time. No message is built with an allocation on the audio thread. |

There is no output limiter: strudel.cc has none, and StrudelDirt's `dirt_monitor` is off by default.

## Comparison with the real StrudelDirt

`tools/sc/compare_strudeldirt.py` plays the same haps in Concha and in the real StrudelDirt:

1. `concha_render --trace-sc` writes the haps that play a StrudelDirt instrument, the controls of each synth started, its envelope, and its effect modules.
2. The haps become what Strudel's OSC output sends (`parseControlsFromHap`).
3. The installed `sclang` runs StrudelDirt (`third_party/strudeldirt`, `tools/sc/strudeldirt_events.scd`) against a fake server, which is a port of that same sclang, and records every message it sends.
4. The instrument's controls, `strudel_envelope`, `dirt_gate` and the effect modules are compared one by one.

```
python3 tools/sc/compare_strudeldirt.py --cycles 10 tests/strudeldirt/*.js
135 synths compared, 0 with differences
```

The three cases in `tests/strudeldirt` cover:

- all 37 instruments by note name, note number and `n`;
- `octave`, `velocity`, `clip`/`legato`, `attack`/`decay`/`hold`/`release`/`curve`/`amp`, `sustain`, `speed`/`accelerate`, `unit("c")`;
- all 21 effect modules.

Two fixes came out of it:

- notes had a 1 ms fade-in and no `envelope` module;
- `dirt_grain_envelope` got its default `curve` instead of StrudelDirt's 0.

One StrudelDirt quirk is reproduced on purpose. Strudel's `hap.duration` already includes `clip`, and StrudelDirt multiplies by `clip` again: `.clip(0.8)` holds the note for 0.64 of its length, on strudel.cc with StrudelDirt as in Concha.

Not compared offline: the sound itself (the same SynthDefs run in the same scsynth 3.13 code), and the orbit effects' messages.

`sclang` is started with an empty config folder by this script and by `compile_synthdefs.sh`. **Your `startup.scd` reboots a server and starts SuperDirt** every time sclang starts: during the first try, a scsynth started on JACK and crashed. It left `/dev/shm/SuperColliderServer_57110`, which Concha doesn't use.

## Checks

- `ctest`: 5 suites, all passing. The `sc` suite covers:
  - the SynthDefs and their controls;
  - a `.scsyndef` file read like the index;
  - a user folder that adds an instrument, which plays in a World created before it;
  - an instrument started at its sample (the same sound 3 samples apart within a block);
  - an effects chain that gives its input back at the same position, and a module (`squiz`) that changes it;
  - `leslie` on one orbit only.
- Measured onsets: with or without StrudelDirt effects, a note starts on the same sample.
- Every instrument plays one note without NaN. Three exceptions, all faithful to StrudelDirt:
  - `dirac`: its single-sample impulse is cancelled by the envelope, which starts at 0;
  - `tutorial5`: it needs `f`;
  - `superhex`: very loud (peak 10), as in StrudelDirt without its limiter.
- The superdough comparison: 111 cases out of 111. The pattern parity suite: 0 differences in 546 cases.
- Cost: idle, the engine and its three Worlds take 1.2 % of a core (242 s of silence rendered in 3 s). A busy pattern (StrudelDirt instruments and effects, 480 notes) renders 18 times faster than real time.

## Known limits

- A voice that reads a bus (`s("bus")`, `bmod`) hears a voice with StrudelDirt effects one block late.
- The sends of a voice with StrudelDirt effects are its values at the start: modulating `delay`/`room` doesn't reach them.
- `leslie`'s sound is 1.3 ms late (scsynth's block), and StrudelDirt's stereo `leslie` gives the same sound left and right, because its two `Balance2` outputs are summed.
- The envelope applies to the instrument's sound before superdough's effects. In StrudelDirt it comes after `shape`/`distort`, which only matters for those non-linear ones.
- A SynthDef folder you remove keeps its SynthDefs until Concha restarts.
- **To test in Bitwig**:
  - StrudelDirt instruments and effects, `leslie` / `shimmer`;
  - a SynthDef folder, rewriting a SynthDef then evaluating;
  - two instances (six Worlds).
