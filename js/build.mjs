// Bundles Strudel (third_party/strudel, unmodified) into one script that QuickJS can run.
// usage: node build.mjs <entry.mjs> <out.js>
import * as esbuild from 'esbuild';
import path from 'node:path';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const packages = path.resolve(here, '../third_party/strudel/packages');
const [entry, out] = process.argv.slice(2);
if (!entry || !out) {
  console.error('usage: node build.mjs <entry.mjs> <out.js>');
  process.exit(1);
}

// Strudel's packages by their package.json name (@strudel/mondo is packages/mondough, mondolang is
// packages/mondo): <name>[/file] -> third_party/strudel/packages/<dir>/(main | file)
const packageDirs = {};
for (const d of fs.readdirSync(packages)) {
  const f = path.join(packages, d, 'package.json');
  if (fs.existsSync(f)) packageDirs[JSON.parse(fs.readFileSync(f, 'utf8')).name] = path.join(packages, d);
}
const strudelPackages = {
  name: 'strudel-packages',
  setup(build) {
    // zzfx_fork.mjs only needs the sample rate from the audio context
    build.onResolve({ filter: /^\.\/audioContext\.mjs$/ }, (args) =>
      args.importer.endsWith('zzfx_fork.mjs') ? { path: path.join(here, 'src/zzfx-context.mjs') } : undefined);
    // hs2js (Tidal) parses with web-tree-sitter (WebAssembly): Concha's C++ tree-sitter instead
    build.onResolve({ filter: /^web-tree-sitter$/ }, () => ({ path: path.join(here, 'src/tree-sitter-native.mjs') }));
    build.onResolve({ filter: /\.wasm\?url$/ }, (args) => ({ path: args.path, namespace: 'wasm-url' }));
    build.onLoad({ filter: /.*/, namespace: 'wasm-url' }, () => ({ contents: 'export default "";', loader: 'js' }));
    build.onResolve({ filter: /^(@strudel\/|mondolang($|\/)|hs2js($|\/))/ }, (args) => {
      const parts = args.path.split('/');
      const scoped = parts[0].startsWith('@');
      const name = scoped ? parts.slice(0, 2).join('/') : parts[0];
      const rest = parts.slice(scoped ? 2 : 1);
      const dir = packageDirs[name] ?? path.join(packages, parts[scoped ? 1 : 0]);
      if (rest.length) return { path: path.join(dir, ...rest) };
      const pkg = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
      return { path: path.join(dir, pkg.main || 'index.mjs') };
    });
  },
};

await esbuild.build({
  entryPoints: [entry],
  outfile: out,
  bundle: true,
  format: 'iife',
  target: 'es2022',
  platform: 'neutral',
  mainFields: ['module', 'main'],
  nodePaths: [path.join(here, 'node_modules')],
  plugins: [strudelPackages],
  logLevel: 'warning',
  legalComments: 'none',
  define: { 'process.env.NODE_ENV': '"production"' },
});
console.log(`wrote ${out} (${(fs.statSync(out).size / 1024).toFixed(0)} KiB)`);
