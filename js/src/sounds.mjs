// Concha's sound registry: what superdough's soundMap, samples(), aliasBank() and soundAlias() do
// (packages/superdough/superdough.mjs, sampler.mjs, synth.mjs; AGPL-3.0, Strudel contributors),
// without WebAudio. At query time, resolveSound() turns a hap value into what the C++ engine
// plays: a synth name, or a sample file (URL / path) with its transposition.
//
// Loading (sample maps, strudel.json) goes through natives provided by Concha (PatternEngine):
//   __concha_fetch(url) -> text      (http(s) with a disk cache, file:// and absolute paths)
//   __concha_library(name) -> JSON   (a named sample folder of the library, as a sample map)
// They are synchronous: they run on the evaluation thread, where waiting is fine.
import { logger, isNote, freqToMidi, getSoundIndex } from '@strudel/core';
import gm from '@strudel/soundfonts/gm.mjs';
import { compileKabel } from './kabel.mjs';
import { getCommonSampleInfo, noteToMidi, midiToFreq } from '@strudel/superdough/util.mjs';
import { buildSamples } from '@strudel/superdough/zzfx_fork.mjs';
import { renderByteBeat, defaultBeats } from './bytebeat.mjs';

// helpers.mjs getFrequencyFromValue (that file needs WebAudio, so it is repeated here).
export function getFrequencyFromValue(value, defaultNote = 36) {
  let { note, freq, octave = 0 } = value;
  note = note || defaultNote;
  if (typeof note === 'string') note = noteToMidi(note);
  if (!freq && typeof note === 'number') freq = midiToFreq(note);
  freq *= Math.pow(2, octave);
  return Number(freq);
}

const soundMap = new Map(); // name (lower case) -> { type, bank?, data }

export function registerSound(key, type, data = {}) {
  key = String(key).toLowerCase().replace(/\s+/g, '_');
  soundMap.set(key, { type, ...data });
}

export function getSound(s) {
  if (typeof s !== 'string') return soundMap.get('triangle');
  return soundMap.get(s.toLowerCase());
}

// synth.mjs registerSynthSounds(): the names superdough knows without samples.
const synths = ['triangle', 'square', 'sawtooth', 'sine', 'user', 'one', 'sbd', 'supersaw', 'bytebeat', 'pulse', 'bus'];
const noises = ['pink', 'white', 'brown', 'crackle'];
for (const s of synths) registerSound(s, 'synth');
for (const s of noises) registerSound(s, 'noise');
// zzfx.mjs registerZZFXSounds()
for (const s of ['zzfx', 'z_sine', 'z_sawtooth', 'z_triangle', 'z_square', 'z_tan', 'z_noise']) registerSound(s, 'zzfx');
for (const [alias, actual] of [['tri', 'triangle'], ['sqr', 'square'], ['saw', 'sawtooth'], ['sin', 'sine']])
  soundMap.set(alias, soundMap.get(actual));
// soundfonts/fontloader.mjs registerSoundfonts() (strudel.cc's prebake): the General MIDI presets,
// fetched from soundfontUrl when first played (disk cache), played by Concha's library and engine
let soundfontUrl = 'https://felixroos.github.io/webaudiofontdata/sound';
export function setSoundfontUrl(url) {
  soundfontUrl = String(url);
}
for (const [name, fonts] of Object.entries(gm)) registerSound(name, 'soundfont', { fonts });

// ---- sample maps (sampler.mjs) ----

function resolveSpecialPaths(base) {
  if (base.startsWith('bubo:')) base = `github:Bubobubobubobubo/dough-${base.split(':')[1]}`;
  return base;
}

function githubPath(base, subpath = '') {
  if (!base.startsWith('github:')) throw new Error('expected "github:" at the start of pseudoUrl');
  let path = base.slice('github:'.length);
  path = path.endsWith('/') ? path.slice(0, -1) : path;
  const c = path.split('/');
  const other = c.slice(3);
  other.push(subpath || '');
  return `https://raw.githubusercontent.com/${c[0]}/${c.length >= 2 ? c[1] : 'samples'}/${c.length >= 3 ? c[2] : 'main'}/${other.join('/')}`;
}

function getBaseURL(url) {
  const i = url.lastIndexOf('/');
  return i >= 0 ? url.slice(0, i) : '';
}

export function processSampleMap(sampleMap, fn, baseUrl = sampleMap._base || '') {
  Object.entries(sampleMap).forEach(([key, value]) => {
    if (key === '_base') return;
    if (typeof value === 'string') value = [value];
    if (typeof value !== 'object') throw new Error('wrong sample map format for ' + key);
    baseUrl = value._base || baseUrl;
    baseUrl = resolveSpecialPaths(baseUrl);
    if (baseUrl.startsWith('github:')) baseUrl = githubPath(baseUrl, '');
    const fullUrl = (v) => baseUrl + v;
    if (Array.isArray(value)) value = value.map(fullUrl);
    else
      value = Object.fromEntries(
        Object.entries(value)
          .filter(([k]) => !k.startsWith('_'))
          .map(([note, s]) => [note, (typeof s === 'string' ? [s] : s).map(fullUrl)]),
      );
    fn(key, value);
  });
}

const canFetch = () => typeof globalThis.__concha_fetch === 'function';
function nativeFetch(url) {
  if (!canFetch()) throw new Error(`cannot load "${url}" here`);
  return globalThis.__concha_fetch(url);
}

// Named sample folders of Concha's library: samples('lib:Name') or samples('Name').
function libraryMap(name) {
  if (typeof globalThis.__concha_library !== 'function') return undefined;
  const json = globalThis.__concha_library(name);
  return json ? JSON.parse(json) : undefined;
}

export function fetchSampleMap(url) {
  const lib = url.startsWith('lib:') ? libraryMap(url.slice(4)) : !/[:/\\.]/.test(url) ? libraryMap(url) : undefined;
  if (lib) return [lib, lib._base || ''];
  if (url.startsWith('lib:')) throw new Error(`no sample folder named "${url.slice(4)}" in Concha's library`);
  url = resolveSpecialPaths(url);
  if (url.startsWith('github:')) url = githubPath(url, 'strudel.json');
  if (url.startsWith('local:')) throw new Error('samples("local:") needs a local server; add the folder to Concha\'s library instead');
  if (url.startsWith('shabda:')) url = `https://shabda.ndre.gr/${url.split('shabda:')[1]}.json?strudel=1`;
  const text = nativeFetch(url);
  let json;
  try {
    json = JSON.parse(text);
  } catch (e) {
    throw new Error(`error loading "${url}": not a sample map (${e.message})`);
  }
  return [json, json._base || getBaseURL(url)];
}

export async function samples(sampleMap, baseUrl = sampleMap?._base || '', options = {}) {
  if (typeof sampleMap === 'string') {
    // Without loader (tests in node), samples('...') does nothing, as superdough does without fetch.
    if (!canFetch() && typeof globalThis.__concha_library !== 'function') return;
    const [json, base] = fetchSampleMap(sampleMap);
    return samples(json, baseUrl || base, options);
  }
  processSampleMap(
    sampleMap,
    (key, bank) => {
      if (key.startsWith('wt_')) registerSound(key, 'wavetable', { bank });
      else registerSound(key, 'sample', { bank });
    },
    baseUrl,
  );
}

// wavetable.mjs tables(url, frameLen, json): like samples(), every key a wavetable of .wav files
export async function tables(url, frameLen, json) {
  let base = '';
  if (json === undefined) {
    if (!canFetch() && typeof globalThis.__concha_library !== 'function') return;
    [json, base] = fetchSampleMap(url);
  } else base = url;
  const baseUrl = json._base || base;
  processSampleMap(
    json,
    (key, bank) => {
      const list = (Array.isArray(bank) ? bank : Object.values(bank).flat()).filter((t) => {
        if (!t.toLowerCase().endsWith('.wav')) {
          logger(`[wavetable] skipping ${t} -- wavetables must be ".wav" format`);
          return false;
        }
        return true;
      });
      if (list.length) registerSound(key, 'wavetable', { bank: list, frameLen });
    },
    baseUrl,
  );
}

export function aliasBank(...args) {
  let aliasMap;
  if (args.length === 1 && typeof args[0] === 'string') aliasMap = JSON.parse(nativeFetch(args[0]));
  else if (args.length === 1) aliasMap = args[0];
  else if (args.length === 2) aliasMap = { [args[0]]: args[1] };
  else throw new Error('aliasMap expects 1 or 2 arguments, received ' + args.length);
  for (const key in aliasMap) aliasMap[key.toLowerCase()] = aliasMap[key];
  for (const [key, sound] of [...soundMap.entries()]) {
    const [bank, suffix] = key.split('_');
    if (!suffix) continue;
    const alias = aliasMap[bank];
    for (const a of typeof alias === 'string' ? [alias] : Array.isArray(alias) ? alias : [])
      soundMap.set(`${a}_${suffix}`.toLowerCase(), sound);
  }
}

export function soundAlias(original, alias) {
  const s = getSound(original);
  if (!s) {
    logger('soundAlias: original sound not found');
    return;
  }
  soundMap.set(String(alias).toLowerCase(), s);
}

export const soundNames = () => [...soundMap.keys()];

// ---- query time ----

const warned = new Set();
function warnOnce(key, message) {
  if (warned.has(key)) return;
  warned.add(key);
  logger(message, 'warning');
}

// zzfx.mjs getZZFX: the parameter list of buildSamples()
function zzfxParams(value) {
  let { s, note = 36, freq, zrand = 0, attack = 0, decay = 0, sustain = 0.8, release = 0.1, curve = 1, slide = 0,
    deltaSlide = 0, pitchJump = 0, pitchJumpTime = 0, lfo = 0, znoise = 0, zmod = 0, zcrush = 0, zdelay = 0,
    tremolo = 0, duration = 0.2, zzfx } = value;
  const sustainTime = Math.max(duration - attack - decay, 0);
  if (typeof note === 'string') note = noteToMidi(note);
  if (!freq && typeof note === 'number') freq = midiToFreq(note);
  s = s.replace('z_', '');
  const shape = ['sine', 'triangle', 'sawtooth', 'tan', 'noise'].indexOf(s) || 0;
  curve = s === 'square' ? 0 : curve;
  return zzfx || [0.25, zrand, freq, attack, sustainTime, release, shape, curve, slide, deltaSlide, pitchJump,
    pitchJumpTime, lfo, znoise, zmod, zcrush, zdelay, sustain, decay, tremolo];
}

const publish = (samples, sampleRate) =>
  typeof globalThis.__concha_buffer === 'function' ? globalThis.__concha_buffer(samples, sampleRate) : 0;

// Writes what the C++ engine needs into the value (keys starting with __):
//   __type: 'synth' | 'noise' | 'sample' | 'wavetable' | 'zzfx' | 'bytebeat' | 'skip'
//   __s: the resolved sound name (bank applied), __url / __transpose / __label for samples,
//   __buffer: the id of a buffer rendered here (zzfx, bytebeat)
// timing: { cps, begin (seconds, Strudel's clock), duration (seconds) }
// superdough(): the impulse response is sample number i (default 1) of the sound named by ir
function irUrl(value) {
  const bank = getSound(value.ir)?.bank;
  if (bank && typeof bank === 'object') {
    const values = Object.values(bank);
    const url = values.flat()[(value.i ?? 1) % values.length];
    if (typeof url === 'string') return url;
  }
  warnOnce(`ir:${value.ir}`, `[superdough] ir: sound ${value.ir} not found! Is it loaded?`);
  return undefined;
}

// StrudelDirt's instruments (the embedded scsynth's SynthDefs), under the names superdough lacks
export function registerSuperDirtSynths(names) {
  for (const name of names) if (!soundMap.has(String(name).toLowerCase())) registerSound(name, 'superdirt');
  return names.length;
}

export function resolveSound(value, timing = {}) {
  let s = value.s ?? 'triangle';
  if (['-', '~', '_'].includes(s)) return { ...value, __type: 'skip' };
  if (value.bank && s) s = `${value.bank}_${s}`;
  const sound = getSound(s);
  if (!sound) {
    warnOnce(`missing:${s}`, `[superdough] sound ${s} not found! Is it loaded?`);
    return { ...value, __type: 'skip' };
  }
  const out = { ...value, s, __type: sound.type, __s: String(s).toLowerCase() };
  if (value.workletSrc !== undefined) {
    try {
      out.__kabel = compileKabel(value, getFrequencyFromValue(value));
    } catch (e) {
      warnOnce(`kabel:${e.message}`, `[kabelsalat] ${e.message}`);
      return { ...value, __type: 'skip' };
    }
  }
  if (Array.isArray(value.FX)) {
    // FX stages: their K() (sFreq = the value's frequency) and ir, as for the value itself
    out.FX = value.FX.map((fx) => {
      if (fx === null || typeof fx !== 'object') return fx;
      const o = { ...fx };
      try {
        if (fx.workletSrc !== undefined) o.__kabel = compileKabel(fx, getFrequencyFromValue(value));
      } catch (e) {
        warnOnce(`kabel:${e.message}`, `[kabelsalat] ${e.message}`);
      }
      if (fx.ir !== undefined && fx.room > 0) o.__irurl = irUrl(fx);
      return o;
    });
  }
  if (value.ir !== undefined && value.room > 0) out.__irurl = irUrl(value);
  const sr = globalThis.__conchaSampleRate ?? 48000;
  if (sound.type === 'zzfx') {
    // superdough passes value.duration (the hap's duration in seconds) to getZZFX
    const samples = buildSamples(...zzfxParams({ ...value, s: out.__s, duration: timing.duration ?? 0.2 }));
    out.__buffer = publish(Float32Array.from(samples), sr);
    if (!out.__buffer) out.__type = 'skip';
    return out;
  }
  if (sound.type === 'synth' && out.__s === 'bytebeat') {
    // synth.mjs 'bytebeat' + byte-beat-processor, rendered for the note's length (8 s at most)
    const { n = 0, byteBeatExpression = defaultBeats[n % defaultBeats.length], byteBeatStartTime } = value;
    const anyAdsr = [value.attack, value.decay, value.sustain, value.release].some((v) => v != null);
    const release = anyAdsr ? Math.max(value.release ?? 0, 0.01) : 0.01;
    const seconds = Math.min(8, (timing.duration ?? 0.25) + release + 0.01);
    let freq;
    try {
      freq = getFrequencyFromValue(value);
    } catch (e) {
      warnOnce(`note:${value.note}`, `[superdough] ${e.message}`);
      return { ...value, __type: 'skip' };
    }
    try {
      const samples = renderByteBeat(byteBeatExpression, freq, timing.begin ?? 0, Math.ceil(seconds * sr), sr, byteBeatStartTime);
      out.__buffer = publish(samples, sr);
    } catch (e) {
      warnOnce(`bytebeat:${byteBeatExpression}`, `[bytebeat] ${e.message}`);
      return { ...value, __type: 'skip' };
    }
    out.__type = 'bytebeat';
    if (!out.__buffer) out.__type = 'skip';
    return out;
  }
  if (sound.type === 'soundfont') {
    // getFontBufferSource: the note's midi number (freq, note name or number; default c3) picks the
    // preset's zone; n picks the preset
    let midi;
    const { note = 'c3', freq } = value;
    if (freq) midi = freqToMidi(freq);
    else if (typeof note === 'string') midi = noteToMidi(note);
    else if (typeof note === 'number') midi = note;
    else {
      warnOnce(`note:${note}`, `[soundfont] unexpected "note" type "${typeof note}"`);
      return { ...value, __type: 'skip' };
    }
    const font = sound.fonts[getSoundIndex(value.n, sound.fonts.length)];
    out.__url = `sf:${soundfontUrl}/${font}.js#${midi}`;
    out.__midi = midi;
    return out;
  }
  if (sound.type === 'superdirt') {
    // osc.mjs parseControlsFromHap + StrudelDirt's defaults: midinote from note (octave 3 for note
    // names), else (n ?? 36) + octave * 12; freq = midinote.midicps unless freq is given
    let midinote;
    if (value.note !== undefined) midinote = isNote(value.note) ? noteToMidi(value.note, value.octave || 3) : Number(value.note);
    else midinote = Number(value.n ?? 36) + Number(value.octave ?? 0) * 12;
    out.__freq = value.freq ?? 440 * Math.pow(2, (midinote - 69) / 12);
    out.__midinote = midinote;
    return out;
  }
  if (sound.type === 'synth') {
    try {
      out.__freq = getFrequencyFromValue(value);
    } catch (e) {
      warnOnce(`note:${value.note}`, `[superdough] ${e.message}`);
      return { ...value, __type: 'skip' };
    }
  }
  if (sound.type === 'sample' || sound.type === 'wavetable') {
    try {
      const { transpose, url, label } = getCommonSampleInfo(value, sound.bank);
      out.__url = url;
      out.__transpose = transpose;
      out.__label = label;
      if (sound.type === 'wavetable') {
        out.__frameLen = sound.frameLen ?? 2048;
        out.__freq = getFrequencyFromValue(value);
      }
    } catch (e) {
      warnOnce(`bank:${s}`, `[sampler] ${s}: ${e.message}`);
      out.__type = 'skip';
    }
  }
  return out;
}
