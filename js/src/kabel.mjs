// K(): kabelsalat graphs (superdough.mjs compileKabel + the generic-processor worklet).
//
// The expression is evaluated and compiled here, on the pattern thread, with kabelsalat's own
// compiler. Its output (the per-sample update code, the list of ugens and the register count) goes
// to the C++ engine, which compiles that code to bytecode and runs it on the audio thread
// (src/superdough/Kabel.cpp), with the ugens ported to C++.
// the same entry as @kabelsalat/lib imports (its dist build): one Node class, one node registry
import * as core from '@kabelsalat/core';
import * as lib from '@kabelsalat/lib/src/lib.js';

let chainId = 0;
const cache = new Map(); // workletSrc after substitutions -> JSON program

// As superdough(): pat[i] -> the i-th input value, sFreq -> the note's frequency, sGate -> a cc
// node the worklet sets to 1 until the note's end.
export function compileKabel(value, freq) {
  const inputs = value.workletInputs ?? [];
  const src = String(value.workletSrc)
    .replace(/\bpat\[(\d+)\]/g, (_, i) => inputs[i])
    .replaceAll('sFreq', freq)
    .replaceAll('sGate', `cc('strudel-gate-${chainId++}')`);
  const key = src.replace(/strudel-gate-\d+/g, 'strudel-gate');
  let json = cache.get(key);
  if (json === undefined) {
    const scope = { ...core, ...lib };
    const node = core.evaluate(src, scope);
    const { src: code, ugens, registers } = node.compile({ log: false });
    json = JSON.stringify({ src: code, ugens: ugens.map((u) => ({ type: u.type, inputs: u.inputs.map(String) })), registers });
    if (cache.size > 256) cache.clear();
    cache.set(key, json);
  }
  return json;
}
