// Phase 0 spike: Strudel's own test runtime (test/runtime.mjs), without @strudel/webaudio,
// bundled for QuickJS. Prints every tune's haps so they can be compared with the snapshots.
import '../src/shims.mjs';
import { evaluate } from '@strudel/transpiler';
import { evalScope } from '@strudel/core';
import * as strudel from '@strudel/core';
import { mini, m } from '@strudel/mini/mini.mjs';
import * as tonalHelpers from '@strudel/tonal';
import * as edoHelpers from '@strudel/edo';
import '@strudel/xen/xen.mjs';
import '../../third_party/strudel/website/src/repl/piano.mjs';
import * as gamepadHelpers from '@strudel/gamepad';
import * as tunes from '../../third_party/strudel/website/src/repl/tunes.mjs';

class MockedNode {
  chain() {
    return this;
  }
  connect() {
    return this;
  }
  toDestination() {
    return this;
  }
  set() {
    return this;
  }
  start() {
    return this;
  }
}

const mockNode = () => new MockedNode();

const id = (x) => x;

const toneHelpersMocked = {
  FeedbackDelay: MockedNode,
  MembraneSynth: MockedNode,
  NoiseSynth: MockedNode,
  MetalSynth: MockedNode,
  Synth: MockedNode,
  PolySynth: MockedNode,
  Chorus: MockedNode,
  Freeverb: MockedNode,
  Gain: MockedNode,
  Reverb: MockedNode,
  vol: mockNode,
  out: id,
  osc: id,
  samples: id,
  adsr: id,
  getDestination: id,
  players: mockNode,
  sampler: mockNode,
  synth: mockNode,
  piano: mockNode,
  polysynth: mockNode,
  fmsynth: mockNode,
  membrane: mockNode,
  noise: mockNode,
  metal: mockNode,
  lowpass: mockNode,
  highpass: mockNode,
};

[
  'osc',
  'csound',
  'tone',
  'webdirt',
  'pianoroll',
  'speak',
  'wave',
  'filter',
  'adsr',
  'webaudio',
  'soundfont',
  'tune',
  'midi',
  '_scope',
  '_spiral',
  '_pitchwheel',
  '_pianoroll',
  '_spectrum',
  'markcss',
  'p',
  'dough',
].forEach((mock) => {
  strudel.Pattern.prototype[mock] = function () {
    return this;
  };
});

const uiHelpersMocked = {
  backgroundImage: id,
};

const canvasCtx = {
  clearRect: () => {},
  fillText: () => {},
  fillRect: () => {},
  canvas: {
    width: 100,
    height: 100,
  },
};
const audioCtx = {
  currentTime: 1,
};
const getDrawContext = () => canvasCtx;
const getAudioContext = () => audioCtx;
const loadSoundfont = () => {};
const loadCsound = () => {};
const loadCSound = () => {};
const loadcsound = () => {};
const getDuration = () => {};

const midin = () => {
  return (ccNum) => strudel.ref(() => 0); // returns ref with default value 0
};

const midikeys = async () => {
  return () => strudel.silence;
};

const sysex = ([id, data]) => {};


// Stand-ins for the @strudel/webaudio exports the tunes call (sounds do not change haps).
const webaudioMocked = { samples: id, aliasBank: id, soundAlias: id, registerSound: id, registerSynthSounds: id };

const scopeReady = evalScope(strudel, toneHelpersMocked, uiHelpersMocked, webaudioMocked, edoHelpers, tonalHelpers, gamepadHelpers, {
  midin, midikeys, sysex, csound: id, loadOrc: id, mini, m, getDrawContext, getAudioContext, loadSoundfont,
  loadCSound, loadCsound, loadcsound, getDuration, setcps: id, setcpm: id, initDough: id, Clock: {},
});

const queryCode = async (code, cycles = 1) => {
  const { pattern } = await evaluate(code);
  return pattern.sortHapsByPart().queryArc(0, cycles).map((h) => h.show(true));
};
const testCycles = {
  timeCatMini: 16,
  timeCat: 8,
  shapeShifted: 16,
  tetrisMini: 16,
  whirlyStrudel: 16,
  swimming: 51,
  giantSteps: 20,
  giantStepsReggae: 25,
  transposedChordsHacked: 8,
  scaleTranspose: 16,
  struct: 4,
  magicSofa: 8,
  confusedPhone: 8,
  zeldasRescue: 48,
  technoDrums: 4,
  caverave: 60,
  callcenterhero: 22,
  primalEnemy: 4,
  synthDrums: 4,
  sampleDrums: 4,
  xylophoneCalling: 60,
  sowhatelse: 60,
  barryHarris: 64,
  wavyKalimba: 64,
  jemblung: 12,
  risingEnemy: 12,
  festivalOfFingers: 16,
  festivalOfFingers2: 22,
  undergroundPlumber: 20,
  bridgeIsOver: 8,
  goodTimes: 16,
  echoPiano: 8,
  sml1: 48,
  speakerman: 48,
  randomBells: 24,
  waa: 16,
  waar: 16,
  festivalOfFingers3: 16,
};

globalThis.queryCode = async (code, cycles) => { await scopeReady; return queryCode(code, cycles); };
globalThis.runTunes = async (only) => {
  await scopeReady;
  const out = {};
  for (const key of Object.keys(tunes)) {
    if (only && key !== only) continue;
    const t0 = performance.now();
    try {
      out[key] = { haps: await queryCode(tunes[key], testCycles[key] || 1) };
    } catch (e) {
      out[key] = { error: String(e && e.stack || e) };
    }
    out[key].ms = performance.now() - t0;
  }
  return out;
};
