# Phase 7 — modules (18 November 2026)

What strudel.cc reaches outside the browser tab, through the plug-in. Your choices:

- **Tidal**: tree-sitter compiled into Concha (downloaded with your agreement);
- **Csound**: later, on its own;
- **Gamepad, serial port, MQTT**: written for Concha, nothing downloaded, Linux first (Windows and macOS in phase 9).

## What is new

| Module | Where | Detail |
|---|---|---|
| **MIDI in: CCs** | `js/src/midiin.mjs`, `ConchaProcessor::readMidiInput` | `const cc = await midin('any name')` then `cc(74)` or `cc(74, 2)` (channel 2): the last value of that CC on the DAW's MIDI input, 0 to 1, read at query time. A plug-in has a single input, the DAW's, so the device name only appears in the console message. The values are **saved with the project**, as strudel.cc keeps them in the browser. |
| **Live controls** (after phase 8, Concha's own) | `concha.mjs liveBindings`, `SoundConvert::parseLive`, `Engine` (modulators of kind 4) | In Strudel an event keeps the values it had at its onset: a knob only reaches the next note. In Concha a parameter that follows `midin` or `macro` also moves **while the note plays**. Each query that reads a control is run again with that control moved by 0.1: a number of a hap's value that changes follows it, with that slope (`__live`: `param:kind:a:b:c0:slope`). The voice gets a modulator of kind 4 on that parameter: `slope × (control now − c0)`, the control read every 128 samples and smoothed over ~10 ms, starting where the control is at the onset. It reaches every parameter lfo/env can modulate (filters and their resonance, gain, velocity, pan, delay and room sends, shape, crush, coarse, distort, phaser, tremolo, delay time/feedback, djf, FM…). Not live: the pitch (a new note follows it), a control that changes the structure (`.fast(cc(1))`), an effect switched off at the onset (`room` at 0), StrudelDirt's instruments and effect modules. |
| **MIDI in: notes** | `midiin.mjs`, `PatternWorker::playMidiNotes` | `const kb = await midikeys('any name')` then `kb()` or `kb("0.25 1")` (note length). As in Strudel, each note-on is played **at once**: the audio thread sends it to the pattern thread, which queries the pattern at that instant (plus 10 ms, Strudel's latency) and keeps the haps from `midikeys`. Values: `note`, `velocity` (0..1), `midichan`. Notes arriving while the DAW is stopped are ignored, as in Strudel. Note-offs are ignored too (fixed note length, as in Strudel). |
| **Macros** | `ConchaProcessor` (parameters "Macro 1" … "Macro 16") | `macro(1)` … `macro(16)`: 16 parameters the DAW can automate or map to a controller, 0 to 1. This one is Concha's own: Strudel has no `macro`. |
| **Soundfonts** | `js/src/sounds.mjs`, `src/library/Soundfont`, `Engine` (`SoundType::Soundfont`) | The General MIDI presets of strudel.cc (`gm_piano`, `gm_epiano1`, `gm_violin`… with `n` picking the variant), available without loading anything, as in strudel.cc. A preset is fetched from felixroos.github.io **the first time it plays**, then kept in the disk cache (`~/.cache/Concha/http`). The note chooses the zone; its MP3 (or PCM) is decoded once per zone, and the notes of a zone share it. The voice plays it like fontloader.mjs: speed from the zone's tuning, the zone's loop, an ADSR peaking at 0.3, then all of superdough. `setSoundfontUrl(url)` changes the source (a local folder works). |
| **Mondo** | `js/build.mjs` | Fixed: `@strudel/mondo` is `packages/mondough` (the language for the transpiler), and `packages/mondo` is `mondolang`. Concha took one for the other, so ``mondo`...` `` could not work. Packages are now found by the name in their package.json. |
| **Tidal** | `src/pattern/HaskellParser`, `js/src/tree-sitter-native.mjs`, `third_party/tree-sitter*` | ``await initTidal()`` then ``tidal`d1 $ s "bd*2" # gain 0.8` ``, as on strudel.cc. Strudel's hs2js parses Haskell with web-tree-sitter (WebAssembly), which QuickJS cannot run. tree-sitter 0.24.7 and tree-sitter-haskell 0.23.1 (Strudel's versions, C sources only) are compiled into Concha, and a stand-in for web-tree-sitter gives hs2js the same tree. The mini-notation inside the Haskell is highlighted while it plays. |
| **Gamepad** | `src/input/Gamepad`, `js/src/shims.mjs` | `gamepad(0)` from Strudel, unchanged: `.x1`, `.y1`, `.x2`, `.y2`, `.a`, `.b`, `.tgla`, `.btnSequence(...)`… Concha provides `navigator.getGamepads()` from a thread that reads gamepads through Linux's evdev (`/dev/input/event*`), with the browser's standard mapping. It opens only the devices that are gamepads (never the keyboard or mouse), finds a pad plugged in later, and supports up to 4 pads. |
| **MQTT** | `js/src/io.mjs`, `src/io/ExternalOut` | `.mqtt(user, password, topic, host, client, latency, add_meta)` and `.robot(id, address)` send the same messages as Strudel (JSON of the value with `duration` and `cps`), at the hap's time. Addresses: `ws://` and `wss://` (MQTT over WebSocket, as Paho in the browser), `mqtt://` and `mqtts://` (plain TCP or TLS). The TCP/TLS connection is libcurl's (already used for samples). The WebSocket and MQTT 3.1.1 parts (connect, publish QoS 0, ping) are written in Concha. A connection opens with the first message, and messages are dropped while it is down, as in Strudel; it is retried every 5 s. **Difference**: there is no password dialog, so give the password in the call. |
| **Serial** | `io.mjs`, `ExternalOut` | `.serial(baud, crc, singlecharids, name)` sends Strudel's messages (`action(key:value,…)` with the optional CRC16, or `key:value…`) 100 ms after the hap, as Strudel. **Difference**: there is no port picker. `name` is the device (`'/dev/ttyACM0'`); with `'default'`, Concha uses the first `/dev/ttyACM*`, then `/dev/ttyUSB*`. |

MQTT and serial messages go out when their part of the song plays. The start prepared while the DAW is stopped waits for Play; a stop or a jump drops the messages already planned.

## Tests

- `ctest`: 5 suites, all passing. New:
  - a note-on played through `midikeys` lands on its sample; `midin` and `macro` read live values;
  - a WebAudioFont preset: zones, base64 PCM, loop, tuning;
  - Tidal (with its mini-notation locations) and Mondo, in QuickJS;
  - MQTT over TCP and over a WebSocket, against a small broker in the test;
  - serial bytes, through a pseudo-terminal.
- A soundfont played from a local preset gives the right pitches (440, 880 and 329.7 Hz).
- The pattern parity suite: 0 differences in 546 cases. The superdough comparison: 111/111.

## What remains

- **To test with real devices** (no gamepad, serial board or MQTT broker here): a gamepad, an Arduino on `/dev/ttyACM0`, a broker (Mosquitto over `ws://` and `mqtt://`), and a real soundfont download from felixroos.github.io.
- **To test in Bitwig**: `midin`/`midikeys` from a controller, the macros (automation and mapping), CC values restored with the project.
- **Not done**:
  - **Csound** (`.csound()`): its own step, as you chose;
  - `loadSoundfont(url)` (`.sf2` files through sfumato): not yet;
  - `midikeys` note-offs (Strudel doesn't use them either);
  - `osc` stays out (your choice in phase 6);
  - gamepad and serial on Windows and macOS (phase 9).
