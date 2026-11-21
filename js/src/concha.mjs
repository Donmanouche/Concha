// Concha's Strudel runtime: the packages that run as plain JavaScript, Strudel's own repl()
// (labels, all/each, hush, solo, setcps...) and a query function the C++ pattern thread calls.
// Bundled by js/build.mjs into concha-strudel.js and run in QuickJS (src/pattern/PatternEngine).
//
// globalThis.concha:
//   ready                          Promise, resolved once the scope is set up
//   evaluate(code)                 Promise<{ ok, error?, miniLocations, widgets, sliders }>
//   query(begin, end, cps)         events with an onset in [begin, end), see toEvent()
//   showHaps(begin, end)           hap.show(true) strings, for comparisons with Strudel's snapshots
//   setSlider(id, value), log()    slider values, log messages since the last call
//   runSuite(cases, only)          parity suite, see js/test/check.mjs
import './shims.mjs';
import * as core from '@strudel/core';
import { repl, evalScope, silence } from '@strudel/core';
import * as mini from '@strudel/mini';
import * as tonal from '@strudel/tonal';
import * as xen from '@strudel/xen';
import * as edo from '@strudel/edo';
import * as mondo from '@strudel/mondo';
import * as tidal from '@strudel/tidal';
import * as gamepad from '@strudel/gamepad';
import * as doughControls from '@strudel/dough/lib.mjs';
import { transpiler, getWidgetID } from '@strudel/transpiler';
import '../../third_party/strudel/website/src/repl/piano.mjs';
import { installStubs, outbox, setSlider, stubGlobals, toEvent } from './stubs.mjs';
import * as midi from './midi.mjs';
import * as midiin from './midiin.mjs';
import * as io from './io.mjs';
import * as canvas from './canvas.mjs';
import * as visuals from './visuals.mjs';
import * as sounds from './sounds.mjs';

let state = { pattern: silence };
let lastError;

const instance = repl({
  transpiler,
  getTime: () => 0,
  setInterval: () => 0,
  clearInterval: () => {},
  onSchedule: () => {},
  onUpdateState: (s) => {
    state = s;
  },
  onEvalError: (e) => {
    lastError = e;
  },
});
// .onPaint() painters (punchcard, pitchwheel...) run by a Drawer, as strudel.cc's editor does
// (codemirror.mjs, drawTime [-2, 2] in useReplContext.jsx), on the background canvas
let drawer;
function startPainters() {
  drawer?.stop();
  drawer = undefined;
  const pattern = state.pattern ?? silence;
  if (!pattern.getPainters().length) return;
  const ctx = visuals.draw.getDrawContext();
  drawer = new visuals.draw.Drawer((haps, time, _, painters) => {
    painters?.forEach((painter) => painter(ctx, time, haps, [-2, 2]));
  }, [-2, 2]);
  drawer.start({ now: () => frameCycle, pattern });
}

// getTime() (draw.mjs's frames): the cycle of the frame being drawn
let frameCycle = 0;
instance.scheduler.now = () => frameCycle;

const ready = (async () => {
  await evalScope(core, mini, tonal, xen, edo, mondo, tidal, gamepad, doughControls, { defaultmidimap: midi.defaultmidimap, midimaps: midi.midimaps },
    { midin: midiin.midin, midikeys: midiin.midikeys, macro: midiin.macro, move: io.move, motor: io.motor },
    visuals.draw,
    { shader: visuals.shader, uniform: visuals.uniform, glsl: visuals.glsl },
    { samples: sounds.samples, tables: sounds.tables, aliasBank: sounds.aliasBank, soundAlias: sounds.soundAlias,
      setSoundfontUrl: sounds.setSoundfontUrl },
    // StrudelDirt's controls that are not Strudel's: shimmer (its README's .shimmer(1)) and its
    // effect modules' parameters (pitch shift, grain envelope, tremolo, distort2)
    ...['shimmer', 'psrate', 'psdisp', 'tilt', 'plat', 'tremolorate', 'distort2'].map((n) => core.registerControl(n)));
  installStubs();
  await evalScope(stubGlobals);
})();

// Folders of Concha's library marked "load at start" are registered before the code runs (like
// strudel.cc's prebake), again whenever the library changes.
let libraryVersion;
function autoloadLibrary() {
  if (typeof globalThis.__concha_library_autoload !== 'function') return;
  const { version, names } = JSON.parse(globalThis.__concha_library_autoload());
  if (version === libraryVersion) return;
  libraryVersion = version;
  sounds.loadLibraryFolders(names); // the folders' sounds again, without those of removed folders
}

// the library changed (a folder added, removed, renamed): its sounds again, without evaluating the
// code; returns the known sounds' names
function refreshLibrary() {
  autoloadLibrary();
  return sounds.soundNames();
}

async function evaluate(code) {
  await ready;
  lastError = undefined;
  autoloadLibrary();
  midiin.resetKeyboards();
  canvas.resetCanvases();
  visuals.resetShader();
  visuals.resetVisuals();
  await instance.evaluate(code, false);
  if (lastError) return { ok: false, error: String(lastError?.message ?? lastError) };
  startPainters();
  return {
    ok: true,
    miniLocations: state.miniLocations ?? [],
    widgets: (state.widgets ?? []).map((w) => ({ type: w.type, from: w.from, to: w.to, value: w.value, canvas: getWidgetID(w) })),
    shader: visuals.takeShader() ?? '',
    visuals: visuals.takeVisuals(),
    sliders: (state.sliders ?? []).map((s) => ({ from: s.from, to: s.to, value: String(s.value), min: s.min, max: s.max, step: s.step })),
  };
}

// What Strudel's Cyclist does each tick (core/cyclist.mjs, core/repl.mjs getSchedule), for one
// query window: haps with an onset, sorted, then the default output unless an onSchedule
// handler is dominant, then that handler.
// keysOnly: the notes just played on the DAW's MIDI input (midiin.mjs, pushKey before).
function query(begin, end, cps, keysOnly) {
  instance.scheduler.cps = cps; // .cpm() reads it
  globalThis.__conchaMouse(); // mousex / mousey
  const pattern = state.pattern ?? silence;
  const run = () =>
    pattern
      .queryArc(begin, end, keysOnly ? { _cps: cps, cyclist: 'concha', conchaKeys: true } : { _cps: cps, cyclist: 'concha' })
      .filter((hap) => hap.hasOnset() && (!keysOnly || hap.value?.midikey !== undefined))
      .sort((a, b) => a.whole.begin.sub(b.whole.begin).valueOf());
  const live = midiin.live;
  live.reads = new Map();
  let haps;
  try {
    haps = run();
  } finally {
    live.reads = live.reads.size ? live.reads : null;
  }
  const bindings = keysOnly ? (live.reads = null) : liveBindings(haps, run);
  const events = [];
  outbox.events = [];
  haps.forEach((hap, index) => {
    const t = hap.whole.begin.valueOf() / cps;
    try {
      let sounded = false;
      if (!hap.context.onSchedule || !hap.context.dominantSchedule) {
        const e = toEvent('sound', hap);
        if (bindings?.[index]?.length) e.value.__live = bindings[index].join(';');
        e.value = sounds.resolveSound(e.value, { cps, begin: e.begin / cps, duration: e.dur / cps });
        if (e.value.__type !== 'skip') {
          events.push(e);
          sounded = true;
        }
      }
      // .midi() and the other outputs send messages, not the hap: the hap itself still goes to the
      // highlighting and to the oscilloscope's channels (kind "visual", nothing plays it).
      if (!sounded && !keysOnly && (hap.context.conchaVisual || hap.context.locations?.length))
        events.push(toEvent('visual', hap));
      if (hap.context.onSchedule) {
        hap.context.onSchedule(hap, t, cps);
        events.push(...outbox.events);
        outbox.events.length = 0;
      }
    } catch (e) {
      globalThis.__conchaLog.push({ message: `[query] ${e?.message ?? e}`, type: 'error' });
    }
  });
  outbox.events = null;
  if (keysOnly) midiin.clearKeys();
  return events;
}

// Parameters that follow a MIDI CC (midin) or a macro: the query again with each control read moved a
// little; a number of a hap's value that changes follows it, with that slope. Returns, per hap,
// "param:kind:a:b:c0:slope" strings; the audio thread then moves the playing voices' parameters with
// the control (Engine: modulators of kind 4). Null when the query read no control.
function liveBindings(haps, run) {
  const live = midiin.live;
  const reads = live.reads;
  live.reads = null;
  if (!reads) return null;
  const bindings = haps.map(() => []);
  for (const [key, c0] of reads) {
    const delta = c0 <= 0.9 ? 0.1 : -0.1;
    live.override = { key, value: c0 + delta };
    let moved;
    try {
      moved = run();
    } catch (e) {
      moved = null;
    } finally {
      live.override = null;
    }
    if (!moved || moved.length !== haps.length) continue; // the control changes the structure: no live binding
    haps.forEach((hap, i) => {
      const other = moved[i];
      if (!other.whole.begin.equals(hap.whole.begin) || typeof hap.value !== 'object' || !hap.value) return;
      for (const [param, v] of Object.entries(hap.value)) {
        const w = other.value?.[param];
        if (typeof v === 'number' && typeof w === 'number' && w !== v && Number.isFinite(w - v))
          bindings[i].push(`${param}:${key}:${c0}:${(w - v) / delta}`);
      }
    });
  }
  return bindings;
}

function showHaps(begin, end) {
  return (state.pattern ?? silence).sortHapsByPart().queryArc(begin, end).map((h) => h.show(true));
}

// Parity suite (js/test/make-cases.mjs): evaluate each case through the repl and list its haps.
async function runSuite(cases, only) {
  const results = {};
  for (const [key, c] of Object.entries(cases)) {
    if (only && !key.includes(only)) continue;
    const t0 = performance.now();
    try {
      const r = await evaluate(c.code);
      results[key] = r.ok ? { haps: showHaps(0, c.cycles) } : { error: r.error };
    } catch (e) {
      results[key] = { error: String(e?.stack ?? e) };
    }
    results[key].ms = performance.now() - t0;
    log();
  }
  return results;
}

// The controls of the SynthDefs played with s("name") that Strudel doesn't have (a user's
// SynthDef with a `harm` control: .harm(2), harm("1 2")), as Strudel's createParam makes them.
function registerSynthControls(names) {
  let n = 0;
  for (const name of names) {
    if (!/^[A-Za-z_$][\w$]*$/.test(name) || name in core.Pattern.prototype || core.controls?.[name]) continue;
    const bag = core.registerControl(name);
    if (globalThis[name] === undefined) globalThis[name] = bag[name];
    ++n;
  }
  return n;
}

function log() {
  const out = globalThis.__conchaLog.splice(0);
  return out;
}

globalThis.concha = {
  ready,
  evaluate,
  query,
  showHaps,
  runSuite,
  log,
  setSlider,
  soundNames: sounds.soundNames,
  refreshLibrary,
  registerSuperDirtSynths: sounds.registerSuperDirtSynths,
  registerSynthControls,
  pushKey: midiin.pushKey,
  // visuals: one frame at `cycle` (canvas.mjs runFrame), the window's size
  frame(cycle, ms) {
    frameCycle = cycle;
    return canvas.runFrame(ms);
  },
  hasFrames: canvas.hasFrames,
  setViewport: canvas.setViewport,
  version: '0.1.0',
};
