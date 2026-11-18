// Builds the parity suite from Strudel's own tests: every tune of website/src/repl/tunes.mjs and
// every documentation @example, with the haps Strudel's snapshots expect.
//   node js/test/make-cases.mjs build/js/cases.json
// Output: { "<key>": { code, cycles, expected: [hap.show(true)...] } }
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const strudel = path.resolve(here, '../../third_party/strudel');
const out = process.argv[2];
if (!out) {
  console.error('usage: make-cases.mjs <cases.json>');
  process.exit(1);
}

function readSnapshots(file) {
  const exportsObj = {};
  vm.runInNewContext(fs.readFileSync(file, 'utf8'), { exports: exportsObj });
  return Object.fromEntries(Object.entries(exportsObj).map(([k, v]) => [k, vm.runInNewContext(v)]));
}

const cases = {};

// --- tunes (test/tunes.test.mjs) ---
const tunes = await import(path.join(strudel, 'website/src/repl/tunes.mjs'));
const runtimeSrc = fs.readFileSync(path.join(strudel, 'test/runtime.mjs'), 'utf8');
const testCycles = vm.runInNewContext('(' + runtimeSrc.match(/export const testCycles = (\{[\s\S]*?\n\});/)[1] + ')');
for (const [k, v] of Object.entries(readSnapshots(path.join(strudel, 'test/__snapshots__/tunes.test.mjs.snap')))) {
  const m = k.match(/^renders tunes > tune: (.+) 1$/);
  if (m && tunes[m[1]]) cases[`tune ${m[1]}`] = { code: tunes[m[1]], cycles: testCycles[m[1]] || 1, expected: v };
}

// --- documentation examples (test/examples.test.mjs, from jsdoc comments) ---
function walkFiles(dir, acc = []) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (/node_modules|shift-|dist/.test(e.name)) continue;
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walkFiles(p, acc);
    else if (/\.(mjs|js|jsx)$/.test(e.name)) acc.push(p);
  }
  return acc;
}
// jsdoc: an @example runs until the next tag; " * " prefixes are stripped.
function parseDoclets(src) {
  const doclets = [];
  const re = /\/\*\*([\s\S]*?)\*\/\s*([^\n]*)/g;
  let m;
  while ((m = re.exec(src))) {
    const lines = m[1].split('\n').map((l) => l.replace(/^\s*\* ?/, ''));
    const text = lines.join('\n');
    const tags = [...text.matchAll(/(^|\n)@(\w+)([^\n]*)((?:\n(?!@)[^\n]*)*)/g)];
    let name = tags.find((t) => t[2] === 'name')?.[3].trim();
    if (!name) {
      const code = m[2];
      name = code.match(/(?:export\s+)?(?:const|let|var|function\*?|class)\s+\{?\s*([\w$]+)/)?.[1] ?? code.match(/^\s*([\w$]+)\s*[:(=]/)?.[1];
    }
    const examples = tags.filter((t) => t[2] === 'example').map((t) => (t[3] + t[4]).replace(/^\s*\n/, '').replace(/\s+$/, ''));
    if (name && examples.length) doclets.push({ name, examples });
  }
  return doclets;
}
const docs = new Map();
for (const f of walkFiles(path.join(strudel, 'packages'))) {
  for (const d of parseDoclets(fs.readFileSync(f, 'utf8'))) if (!docs.has(d.name)) docs.set(d.name, d.examples);
}
let missing = 0;
for (const [k, v] of Object.entries(readSnapshots(path.join(strudel, 'test/__snapshots__/examples.test.mjs.snap')))) {
  const m = k.match(/^runs examples > example "(.+)" example index (\d+) 1$/);
  if (!m) continue;
  const code = docs.get(m[1])?.[Number(m[2])];
  if (code === undefined) {
    missing++;
    continue;
  }
  cases[`example ${m[1]} #${m[2]}`] = { code, cycles: 4, expected: v };
}

fs.writeFileSync(out, JSON.stringify(cases));
const n = Object.keys(cases).length;
console.log(`wrote ${out}: ${n} cases (${Object.keys(cases).filter((k) => k.startsWith('tune')).length} tunes), ${missing} snapshot examples not found in the sources`);
