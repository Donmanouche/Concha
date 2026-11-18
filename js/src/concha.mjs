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
import * as gamepad from '@strudel/gamepad';
import * as doughControls from '@strudel/dough/lib.mjs';
import { transpiler } from '@strudel/transpiler';
import '../../third_party/strudel/website/src/repl/piano.mjs';
import { installStubs, outbox, setSlider, stubGlobals, toEvent } from './stubs.mjs';
import * as midi from './midi.mjs';
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

const ready = (async () => {
  await evalScope(core, mini, tonal, xen, edo, mondo, gamepad, doughControls, { defaultmidimap: midi.defaultmidimap, midimaps: midi.midimaps },
    { samples: sounds.samples, tables: sounds.tables, aliasBank: sounds.aliasBank, soundAlias: sounds.soundAlias });
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
  for (const name of names) {
    try {
      sounds.samples('lib:' + name);
    } catch (e) {
      globalThis.__conchaLog.push({ message: `[library] ${name}: ${e?.message ?? e}`, type: 'error' });
    }
  }
}

async function evaluate(code) {
  await ready;
  lastError = undefined;
  autoloadLibrary();
  await instance.evaluate(code, false);
  if (lastError) return { ok: false, error: String(lastError?.message ?? lastError) };
  return {
    ok: true,
    miniLocations: state.miniLocations ?? [],
    widgets: (state.widgets ?? []).map((w) => ({ type: w.type, from: w.from, to: w.to, value: w.value })),
    sliders: (state.sliders ?? []).map((s) => ({ from: s.from, to: s.to, value: String(s.value), min: s.min, max: s.max, step: s.step })),
  };
}

// What Strudel's Cyclist does each tick (core/cyclist.mjs, core/repl.mjs getSchedule), for one
// query window: haps with an onset, sorted, then the default output unless an onSchedule
// handler is dominant, then that handler.
function query(begin, end, cps) {
  instance.scheduler.cps = cps; // .cpm() reads it
  const pattern = state.pattern ?? silence;
  const haps = pattern
    .queryArc(begin, end, { _cps: cps, cyclist: 'concha' })
    .filter((hap) => hap.hasOnset())
    .sort((a, b) => a.whole.begin.sub(b.whole.begin).valueOf());
  const events = [];
  outbox.events = [];
  for (const hap of haps) {
    const t = hap.whole.begin.valueOf() / cps;
    try {
      if (!hap.context.onSchedule || !hap.context.dominantSchedule) {
        const e = toEvent('sound', hap);
        e.value = sounds.resolveSound(e.value, { cps, begin: e.begin / cps, duration: e.dur / cps });
        if (e.value.__type !== 'skip') events.push(e);
      }
      if (hap.context.onSchedule) {
        hap.context.onSchedule(hap, t, cps);
        events.push(...outbox.events);
        outbox.events.length = 0;
      }
    } catch (e) {
      globalThis.__conchaLog.push({ message: `[query] ${e?.message ?? e}`, type: 'error' });
    }
  }
  outbox.events = null;
  return events;
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
  version: '0.1.0',
};
