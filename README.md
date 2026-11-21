# Concha

**[Strudel](https://strudel.cc) as a CLAP and VST3 plug-in.** Write Strudel code inside your DAW: the
patterns follow the host's tempo and transport, and the sound comes out of the plug-in.

![Concha in a DAW](docs/screenshot.png)

## Features

- **Strudel, in your DAW.** The real Strudel language (mini-notation, tonal, signals, all
  functions), with a strudel.cc-like editor: highlighting of the playing notes, reference, sounds,
  sliders, scope and pianoroll. Synths and effects are superdough's, rewritten in C++.
- **Multichannel audio out.** 16 stereo outputs: `.orbit(n)` goes to the output *Orbit n* (orbits 0
  and 1 to *Main*), so each part can get its own track, mixer channel and effects in the DAW.
- **MIDI in and out.**
  - `.midi()` sends notes, CC, program change, pitch bend, NRPN and sysex to the DAW, on the exact
    sample, to drive any instrument or hardware.
  - `midin()` reads the MIDI CCs coming from the DAW. Parameters bound to a knob (filter, gain,
    pan, effects…) move **in real time**, even while a note plays. `midikeys()` plays the pattern
    from the notes you play. 16 automatable macros: `macro(1)` … `macro(16)`.
- **Your own samples, from your disk.** In *Sounds → Sample folders…*, add folders and give them a
  name. Their sounds are available at once: `s("kick snare")`, `s("bd*4").bank("MyDrums")`,
  `n("0 1 2").s("pads")`. Dirt-Samples, tidal-drum-machines and `strudel.json` layouts are
  understood. `samples('github:…')` and URLs work too, cached on disk.

Also inside: SuperCollider's StrudelDirt instruments and effects (embedded scsynth, nothing to
install), GM soundfonts, Tidal syntax, Mondo, gamepad, MQTT and serial output, shaders.

## Install (Linux x86_64)

1. Download `Concha-0.1-linux-x86_64.zip` from the
   [Releases](../../releases) page.
2. Copy `Concha.clap` to `~/.clap/` and/or `Concha.vst3` to `~/.vst3/`.
3. Rescan plug-ins in your DAW. Tested in Bitwig Studio (CLAP recommended) and Carla.

Runs on Ubuntu 22.04 and later (glibc 2.35). Windows and macOS versions are planned.

## Quick start

- **Ctrl+Enter** evaluates the code, **Ctrl+.** (or *stop*) silences it. After a stop, the next
  Ctrl+Enter starts the piece from its first cycle.
- The plug-in plays while the DAW plays. One cycle = 4 beats by default (*Beats per cycle*
  parameter). Turn on *Free run* to play with the transport stopped.
- Route the extra outputs (*Orbit 2* … *Orbit 16*) to tracks in your DAW to use `.orbit()`.

```js
const cc = await midin('knobs')
$: note("<c2 g1>").s("sawtooth").lpf(cc(74).range(200, 5000)).orbit(2)
$: s("bd*4, [~ sd]*2").bank("MyDrums")
```

## Building from source

CMake, Ninja, a C++20 compiler and Node.js. JUCE 8, clap-juce-extensions, the SuperCollider and
sc3-plugins sources, dr_libs and stb go in `../deps`. Strudel and QuickJS-ng are cloned in
`third_party/`. Then:

```sh
npm --prefix js ci
cmake -S . -B build -G Ninja && ninja -C build
```

Details are in [docs/](docs/). Release builds for Ubuntu 22.04: `tools/release/build-jammy.sh`.

## License

AGPL-3.0-or-later, like Strudel. Concha bundles Strudel (uzu/strudel, AGPL-3.0), QuickJS-ng,
StrudelDirt, SuperCollider, tree-sitter and JUCE, under their own licenses.
