// Strudel's function reference for Concha's editor (help panel and completion), from the JSDoc
// comments of third_party/strudel/packages, as Strudel's own doc.json (jsdoc-json) holds them:
// name, synonyms, description, parameters, examples, tags.
//
//   node js/tools/gen-docs.mjs <strudel/packages> <out.json>
//
// A small JSDoc reader (no jsdoc download): description before the first tag, @name (or the name
// of the declaration that follows), @synonyms, @param {type} name text, @example blocks, @tags,
// @noAutocomplete / @superdirtOnly. Entries are sorted by name.
import fs from 'node:fs';
import path from 'node:path';

const [root, out] = process.argv.slice(2);
if (!root || !out) {
  console.error('usage: node gen-docs.mjs <strudel/packages> <out.json>');
  process.exit(1);
}

const files = [];
(function walk(dir) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) {
      if (/node_modules|dist|shift-|test|__tests__/.test(e.name)) continue;
      walk(p);
    } else if (/\.(mjs|js|jsx)$/.test(e.name)) files.push(p);
  }
})(root);
files.sort();

const stripHtml = (s) =>
  s
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<[^>]+>/g, '')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&amp;/g, '&')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/\[([^\]]+)\]\((https?:[^)]+)\)/g, '$1 ($2)')
    .replace(/\{@link\s+([^}\s|]+)(?:\s*\|\s*([^}]+))?\}/g, (_, a, b) => b || a);
// HTML out, and the source's hard line breaks joined inside paragraphs (lists, tables, indented
// and fenced code keep theirs)
function tidy(s) {
  const lines = stripHtml(s).split('\n').map((l) => l.trimEnd());
  const out = [];
  let fence = false;
  const keeps = (l) => /^\s*([-*+]\s|\d+[.)]\s|\||#|>|```)/.test(l) || /^\s{2,}\S/.test(l);
  for (const l of lines) {
    if (/^\s*```/.test(l)) fence = !fence;
    const prev = out.length ? out[out.length - 1] : '';
    if (!fence && l.trim() && prev.trim() && !keeps(l) && !/^\s*```/.test(prev) && !/^\s{2,}\S/.test(prev) && !/^\s*\|/.test(prev))
      out[out.length - 1] = prev + ' ' + l.trim();
    else out.push(l);
  }
  return out.join('\n').replace(/\n{3,}/g, '\n\n').trim();
}

// the declaration after a comment: export const foo =, function foo(, Pattern.prototype.foo =, foo: ...
function declaredName(after) {
  const m =
    after.match(/^\s*(?:export\s+)?(?:async\s+)?function\s*\*?\s*([A-Za-z_$][\w$]*)/) ||
    after.match(/^\s*(?:export\s+)?(?:const|let|var)\s+([A-Za-z_$][\w$]*)/) ||
    after.match(/^\s*(?:export\s+)?(?:const|let)\s*\{\s*([A-Za-z_$][\w$]*)/) ||
    after.match(/^\s*[\w$.]*prototype\.([A-Za-z_$][\w$]*)\s*=/) ||
    after.match(/^\s*([A-Za-z_$][\w$]*)\s*[:(]/);
  return m?.[1];
}

const docs = new Map();
for (const file of files) {
  const src = fs.readFileSync(file, 'utf8');
  const re = /\/\*\*([\s\S]*?)\*\//g;
  let m;
  while ((m = re.exec(src))) {
    const lines = m[1].split('\n').map((l) => l.replace(/^\s*\* ?/, ''));
    const doc = { name: undefined, synonyms: [], description: '', params: [], examples: [], tags: [], exclude: false };
    let current = null; // the tag whose text continues on the next lines
    const desc = [];
    for (const line of lines) {
      const t = line.match(/^@(\w+)\s?(.*)$/);
      if (!t) {
        if (current) current.text.push(line);
        else desc.push(line);
        continue;
      }
      const [, tag, rest] = t;
      current = { tag, text: [rest] };
      if (tag === 'name') doc.name = rest.trim().split(/\s+/)[0];
      else if (tag === 'synonyms') doc.synonyms.push(...rest.split(/[,\s]+/).filter(Boolean));
      else if (tag === 'param') doc.params.push(current);
      else if (tag === 'example') doc.examples.push(current);
      else if (tag === 'tags') doc.tags.push(...rest.split(/[,\s]+/).filter(Boolean));
      else if (tag === 'noAutocomplete' || tag === 'superdirtOnly') doc.exclude = true;
      else if (tag === 'private' || tag === 'ignore') doc.exclude = 'hidden';
    }
    if (doc.exclude === 'hidden') continue;
    doc.name ??= declaredName(src.slice(re.lastIndex, re.lastIndex + 300));
    if (!doc.name || doc.name.startsWith('_')) continue;
    const params = doc.params.map((p) => {
      const text = p.text.join('\n');
      const pm = text.match(/^\s*(?:\{([^}]*)\})?\s*(\[[^\]]*\]|[^\s]+)?\s*-?\s*([\s\S]*)$/);
      let name = (pm?.[2] ?? '').replace(/^\[|\]$/g, '').split('=')[0];
      return [name, (pm?.[1] ?? '').trim(), tidy(pm?.[3] ?? '')];
    });
    const entry = {
      n: doc.name,
      s: doc.synonyms,
      d: tidy(desc.join('\n')),
      p: params,
      e: doc.examples.map((e) => e.text.join('\n').replace(/^\n+|\s+$/g, '')).filter(Boolean),
      t: doc.tags,
      x: doc.exclude === true,
    };
    // a later, richer comment for the same name wins (signatures repeated in several files)
    const prev = docs.get(entry.n);
    if (!prev || (entry.d.length + entry.e.length * 40 > prev.d.length + prev.e.length * 40)) docs.set(entry.n, entry);
  }
}

const list = [...docs.values()].sort((a, b) => a.n.localeCompare(b.n));
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, JSON.stringify(list));
console.log(`wrote ${out}: ${list.length} entries`);
