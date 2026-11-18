// Compares tune haps with Strudel's own snapshots (test/__snapshots__/tunes.test.mjs.snap).
// usage: node spike/compare.mjs --bundle build/js/tunes.js     (runs the bundle in node)
//        node spike/compare.mjs --json results.json            (results printed by QuickJS)
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const snapFile = path.resolve(here, '../../third_party/strudel/test/__snapshots__/tunes.test.mjs.snap');

// Same rounding as tunes.test.mjs.
const roundFloatStrings = (input, precision = 12) =>
  input.replace(/-?\d+\.\d+/g, (m) => Number(m).toFixed(precision).replace(/\.?0+$/, '').replace(/\.$/, ''));

const snapshots = {};
vm.runInNewContext(fs.readFileSync(snapFile, 'utf8'), { exports: snapshots });
const expected = {};
for (const [k, v] of Object.entries(snapshots)) {
  const m = k.match(/^renders tunes > tune: (.+) 1$/);
  if (m) expected[m[1]] = vm.runInNewContext(v);
}

const [mode, file] = process.argv.slice(2);
let results;
if (mode === '--bundle') {
  vm.runInThisContext(fs.readFileSync(file, 'utf8'));
  results = await globalThis.runTunes();
} else if (mode === '--json') {
  results = JSON.parse(fs.readFileSync(file, 'utf8'));
} else {
  console.error('usage: compare.mjs --bundle <tunes.js> | --json <results.json>');
  process.exit(1);
}

let same = 0, differ = 0, failed = 0, totalMs = 0;
for (const [key, want] of Object.entries(expected)) {
  const got = results[key];
  if (!got) { console.log(`MISSING ${key}`); failed++; continue; }
  totalMs += got.ms || 0;
  if (got.error) { console.log(`ERROR   ${key}: ${got.error.split('\n')[0]}`); failed++; continue; }
  const haps = got.haps.map((h) => roundFloatStrings(h));
  const n = Math.max(haps.length, want.length);
  let firstDiff = -1;
  for (let i = 0; i < n; i++) if (haps[i] !== want[i]) { firstDiff = i; break; }
  if (firstDiff === -1) { same++; continue; }
  differ++;
  console.log(`DIFF    ${key}: ${haps.length} vs ${want.length} haps, first at #${firstDiff}`);
  console.log(`          got:  ${haps[firstDiff]}`);
  console.log(`          want: ${want[firstDiff]}`);
}
console.log(`\n${same} identical, ${differ} different, ${failed} failed, of ${Object.keys(expected).length} tunes; query time ${totalMs.toFixed(0)} ms`);
