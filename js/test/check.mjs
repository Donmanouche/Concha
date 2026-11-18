// Compares Concha's haps with Strudel's snapshots (cases from make-cases.mjs).
//   node js/test/check.mjs <cases.json> --bundle <concha-strudel.js> [filter]   run in node
//   node js/test/check.mjs <cases.json> --json <results.json>                   results of concha_query --suite
import fs from 'node:fs';
import vm from 'node:vm';

const [casesFile, mode, file, only] = process.argv.slice(2);
const cases = JSON.parse(fs.readFileSync(casesFile, 'utf8'));
let results;
if (mode === '--bundle') {
  // A fresh runtime per case, like a freshly opened page: some examples change global settings
  // (setDefaultJoin, useRNG...) that would leak into the next ones.
  const bundle = new vm.Script(fs.readFileSync(file, 'utf8'));
  const quiet = { log() {}, warn() {}, error() {}, info() {}, debug() {} };
  results = {};
  for (const [key, c] of Object.entries(cases)) {
    if (only && !key.includes(only)) continue;
    const ctx = vm.createContext({ console: quiet });
    bundle.runInContext(ctx);
    const r = await vm.runInContext(`concha.ready.then(() => concha.runSuite(${JSON.stringify({ [key]: c })}))`, ctx);
    results[key] = JSON.parse(JSON.stringify(r[key]));
  }
} else if (mode === '--json') {
  results = JSON.parse(fs.readFileSync(file, 'utf8'));
} else {
  console.error('usage: check.mjs <cases.json> --bundle <bundle.js> [filter] | --json <results.json>');
  process.exit(1);
}

// Same rounding as Strudel's tunes.test.mjs (float formatting differs between JS engines).
const round = (s) => s.replace(/-?\d+\.\d+/g, (m) => Number(m).toFixed(12).replace(/\.?0+$/, '').replace(/\.$/, ''));

const known = JSON.parse(fs.readFileSync(new URL('./known-differences.json', import.meta.url), 'utf8'));
const verbose = process.env.VERBOSE === '1';
let same = 0, nearly = 0, expected = 0, differ = 0, failed = 0, ms = 0;
const lines = [];
for (const [key, c] of Object.entries(cases)) {
  const r = results[key];
  if (!r) continue;
  ms += r.ms ?? 0;
  if (r.error) {
    failed++;
    lines.push(`ERROR  ${key}: ${r.error.split('\n')[0]}`);
    continue;
  }
  // hap.show() goes through JSON.stringify, so a real line break in an expected string comes from
  // reading the .snap file (vitest stores \\n unescaped): put the escape back.
  const got = r.haps.map(round), want = c.expected.map((h) => round(h.replace(/\n/g, '\\n')));
  let first = -1;
  for (let i = 0; i < Math.max(got.length, want.length); i++) if (got[i] !== want[i]) { first = i; break; }
  if (first === -1) { same++; continue; }
  if (known[key]) { expected++; if (verbose) lines.push(`KNOWN  ${key}: ${known[key]}`); continue; }
  // Floats may differ in the last digits between QuickJS and V8: compare numbers with a tolerance.
  const nums = (s) => (s ?? '').split(/(-?\d+(?:\.\d+)?(?:e-?\d+)?)/);
  const close = got.length === want.length && got.every((g, i) => {
    const a = nums(g), b = nums(want[i]);
    return a.length === b.length && a.every((x, j) => (j % 2 ? Math.abs(Number(x) - Number(b[j])) <= 1e-9 * Math.max(1, Math.abs(Number(x))) : x === b[j]));
  });
  if (close) { nearly++; if (verbose) lines.push(`~      ${key}`); continue; }
  differ++;
  lines.push(`DIFF   ${key}: ${got.length} vs ${want.length} haps, first at #${first}\n         got:  ${got[first]}\n         want: ${want[first]}`);
}
console.log(lines.join('\n'));
const total = same + nearly + expected + differ + failed;
console.log(`\n${same} identical, ${nearly} identical within 1e-9, ${expected} known differences (test runtime, see known-differences.json), ${differ} different, ${failed} errors, of ${total} cases; ${ms.toFixed(0)} ms`);
process.exitCode = differ + failed ? 1 : 0;
