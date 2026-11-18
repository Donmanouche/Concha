// Lists the public API of the Strudel packages that need a browser (WebAudio, canvas, WebMIDI,
// network...). Concha cannot load them, but user code may call their functions, so
// js/src/stubs.mjs defines each name. Real native implementations replace the stubs
// phase by phase. Run again after updating third_party/strudel:
//   node js/tools/gen-browser-api.mjs > js/src/generated/browser-api.json
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from 'acorn';

const here = path.dirname(fileURLToPath(import.meta.url));
const packagesDir = path.resolve(here, '../../third_party/strudel/packages');

// Packages strudel.cc puts in scope (website/src/repl/util.mjs loadModules) that Concha does not run as JS.
const browserPackages = ['draw', 'webaudio', 'codemirror', 'hydra', 'serial', 'soundfonts', 'csound', 'motion', 'mqtt', 'dough', 'midi', 'osc', 'tidal'];

function resolveModule(spec, fromFile) {
  if (spec.startsWith('.')) {
    let p = path.resolve(path.dirname(fromFile), spec);
    if (!fs.existsSync(p) && fs.existsSync(p + '.mjs')) p += '.mjs';
    return p;
  }
  const bare = spec.replace(/^@strudel\//, '');
  const [name, ...rest] = bare.split('/');
  const dir = path.join(packagesDir, name);
  if (!fs.existsSync(path.join(dir, 'package.json'))) return null; // npm dependency: not part of the API
  if (rest.length) return path.join(dir, ...rest);
  return path.join(dir, JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).main || 'index.mjs');
}

const literal = (node) => (node && node.type === 'Literal' && typeof node.value !== 'object' ? node.value : undefined);

function namesInPattern(p, out) {
  if (!p) return;
  if (p.type === 'Identifier') out.push(p.name);
  else if (p.type === 'ObjectPattern') p.properties.forEach((q) => namesInPattern(q.value ?? q.argument, out));
  else if (p.type === 'ArrayPattern') p.elements.forEach((q) => namesInPattern(q, out));
  else if (p.type === 'AssignmentPattern') namesInPattern(p.left, out);
}

function walk(node, f) {
  if (!node || typeof node.type !== 'string') return;
  f(node);
  for (const k in node) {
    const v = node[k];
    if (Array.isArray(v)) v.forEach((c) => walk(c, f));
    else if (v && typeof v.type === 'string') walk(v, f);
  }
}

// Collects exports (following re-exports inside Strudel packages) and register('name') calls.
function scan(entry, api, seen = new Set()) {
  if (!entry || seen.has(entry) || !fs.existsSync(entry) || !/\.m?js$/.test(entry)) return;
  seen.add(entry);
  let ast;
  try {
    ast = parse(fs.readFileSync(entry, 'utf8'), { ecmaVersion: 'latest', sourceType: 'module' });
  } catch (e) {
    console.error(`skip ${entry}: ${e.message}`);
    return;
  }
  for (const node of ast.body) {
    if (node.type === 'ExportAllDeclaration') scan(resolveModule(node.source.value, entry), api, seen);
    if (node.type === 'ExportNamedDeclaration') {
      const d = node.declaration;
      if (d?.type === 'FunctionDeclaration') api.exports[d.id.name] ??= { kind: 'function' };
      else if (d?.type === 'ClassDeclaration') api.exports[d.id.name] ??= { kind: 'class' };
      else if (d?.type === 'VariableDeclaration') {
        for (const decl of d.declarations) {
          const names = [];
          namesInPattern(decl.id, names);
          const init = decl.init;
          const value = names.length === 1 ? literal(init) : undefined;
          for (const n of names) {
            if (value !== undefined) api.exports[n] ??= { kind: 'value', value };
            else if (init && /Function|Arrow/.test(init.type)) api.exports[n] ??= { kind: 'function' };
            else api.exports[n] ??= { kind: 'object' };
          }
        }
      }
      for (const s of node.specifiers ?? []) api.exports[s.exported.name ?? s.exported.value] ??= { kind: 'object' };
      if (node.source) scan(resolveModule(node.source.value, entry), api, seen);
    }
    if (node.type === 'ImportDeclaration') {
      const target = resolveModule(node.source.value, entry);
      if (target && target.startsWith(path.dirname(entry))) scan(target, api, seen); // side-effect files of the same package
    }
  }
  walk(ast, (n) => {
    if (n.type === 'CallExpression' && n.callee.type === 'Identifier' && n.callee.name === 'registerWidget') {
      const a = literal(n.arguments[0]);
      if (a !== undefined) api.widgets.add(String(a));
    }
    if (n.type === 'CallExpression' && n.callee.type === 'Identifier' && n.callee.name === 'register') {
      const a = n.arguments[0];
      if (literal(a) !== undefined) api.methods.add(String(literal(a)));
      if (a?.type === 'ArrayExpression') a.elements.forEach((e) => literal(e) !== undefined && api.methods.add(String(literal(e))));
    }
    // Pattern.prototype.name = ...
    if (n.type === 'AssignmentExpression' && n.left.type === 'MemberExpression') {
      const o = n.left.object;
      if (o.type === 'MemberExpression' && o.object.name === 'Pattern' && o.property.name === 'prototype' && n.left.property.type === 'Identifier')
        api.methods.add(n.left.property.name);
    }
  });
}

const result = {};
for (const name of browserPackages) {
  const api = { exports: {}, methods: new Set(), widgets: new Set() };
  scan(resolveModule(name, path.join(packagesDir, 'x.mjs')), api);
  result[name] = { exports: Object.fromEntries(Object.entries(api.exports).sort()), methods: [...api.methods].sort(), widgets: [...api.widgets].sort() };
}
process.stdout.write(JSON.stringify(result, null, 1) + '\n');
