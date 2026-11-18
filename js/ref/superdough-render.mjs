// Renders Strudel code with the REAL superdough (third_party/strudel/packages/superdough) in node,
// on node-web-audio-api's OfflineAudioContext. Reference for tests/ref/compare.py.
//   node build/js/superdough-render.mjs <code.js> <out.wav> [cycles] [cps]
// Bundled by js/ref/build-ref.mjs (worklet imports are stubbed: only features without
// AudioWorklets are compared for now).
import * as nwa from 'node-web-audio-api';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

import '../src/shims.mjs'; // document / window stand-ins, as in Concha
import path from 'node:path';
import * as kabelCore from '@kabelsalat/core';
import * as kabelLib from '@kabelsalat/lib/src/lib.js';

// Node 20 lacks Promise.withResolvers (Node 22), used by node-web-audio-api's AudioWorklet.
if (!Promise.withResolvers)
  Promise.withResolvers = () => {
    let resolve, reject;
    const promise = new Promise((a, b) => ((resolve = a), (reject = b)));
    return { promise, resolve, reject };
  };

// Deterministic reverb noise, identical to Concha's (Engine::getOrbit seed for orbit 1, xorshift32)
{
  let next = Math.imul(0x9e3779b9, 1 + 7) >>> 0, s = 1;
  globalThis.__conchaReverbStart = () => {
    s = (next | 1) >>> 0;
    next = (next + 1) >>> 0;
  };
  globalThis.__conchaReverbRandom = () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

// superdough uses the WebAudio classes as globals (new GainNode(ac, ...), ConstantSourceNode...)
Object.assign(globalThis, nwa);
// SalatRepl({ localScope: true }).evaluate without its browser audio view
globalThis.__conchaKabel = { evaluate: (code) => kabelCore.evaluate(code, { ...kabelCore, ...kabelLib }) };

// fetch() for file:// URLs (samples given as files)
const realFetch = globalThis.fetch;
globalThis.fetch = async (url, ...rest) => {
  const u = String(url);
  if (u.startsWith('file://')) {
    const data = fs.readFileSync(fileURLToPath(u.replace('%23', '#')));
    return {
      ok: true,
      arrayBuffer: async () => data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength),
      json: async () => JSON.parse(data.toString('utf8')),
    };
  }
  return realFetch(url, ...rest);
};

const core = await import('@strudel/core');
const mini = await import('@strudel/mini');
const tonal = await import('@strudel/tonal');
const { evaluate } = await import('@strudel/transpiler');
const sd = await import('@strudel/superdough');
const { SuperdoughAudioController } = await import('@strudel/superdough/superdoughoutput.mjs');

const [codeFile, outFile, cyclesArg = '4', cpsArg = '0.5'] = process.argv.slice(2);
const cycles = Number(cyclesArg), cps = Number(cpsArg), rate = 48000;
const length = Math.ceil((cycles / cps + 2) * rate);

const ctx = new nwa.OfflineAudioContext(2, length, rate);
// superdough's AudioWorklets (worklets.mjs, bundled by js/ref/build-worklets.mjs)
await ctx.audioWorklet.addModule(path.join(path.dirname(fileURLToPath(import.meta.url)), 'worklets.mjs'));
sd.setAudioContext(ctx);
sd.setSuperdoughAudioController(new SuperdoughAudioController(ctx));
sd.registerSynthSounds();
sd.registerZZFXSounds(); // strudel.cc's prebake registers them too
await core.evalScope(core, mini, tonal, { samples: sd.samples, aliasBank: sd.aliasBank, soundAlias: sd.soundAlias });

const { pattern } = await evaluate(fs.readFileSync(codeFile, 'utf8'));
const haps = pattern.queryArc(0, cycles).filter((h) => h.hasOnset());
let played = 0;
for (const hap of haps) {
  hap.ensureObjectValue();
  const t = hap.whole.begin.valueOf() / cps;
  try {
    await sd.superdough({ ...hap.value }, t, hap.duration.valueOf() / cps, cps, hap.whole.begin.valueOf());
    played++;
  } catch (e) {
    console.error(`[superdough] ${e.message}`);
  }
}
// superdough builds reverb impulse responses asynchronously (applyGradualLowpass renders them in
// another OfflineAudioContext): in a browser they are ready within milliseconds, here the main
// render would start before. Let those renders finish first.
await new Promise((r) => setTimeout(r, 1500));
const buffer = await ctx.startRendering();

// 32-bit float WAV
const ch = [buffer.getChannelData(0), buffer.getChannelData(1)];
const n = buffer.length;
const out = Buffer.alloc(44 + n * 8);
out.write('RIFF', 0);
out.writeUInt32LE(36 + n * 8, 4);
out.write('WAVEfmt ', 8);
out.writeUInt32LE(16, 16);
out.writeUInt16LE(3, 20);
out.writeUInt16LE(2, 22);
out.writeUInt32LE(rate, 24);
out.writeUInt32LE(rate * 8, 28);
out.writeUInt16LE(8, 32);
out.writeUInt16LE(32, 34);
out.write('data', 36);
out.writeUInt32LE(n * 8, 40);
for (let i = 0; i < n; i++) {
  out.writeFloatLE(ch[0][i], 44 + i * 8);
  out.writeFloatLE(ch[1][i], 48 + i * 8);
}
fs.writeFileSync(outFile, out);
console.error(`${played} haps, ${(n / rate).toFixed(1)} s`);
process.exit(0); // the worklet threads would keep node running
