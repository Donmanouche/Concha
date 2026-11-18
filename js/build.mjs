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

// @strudel/<name>[/file] -> third_party/strudel/packages/<name>/(index.mjs | file)
const strudelPackages = {
  name: 'strudel-packages',
  setup(build) {
    // zzfx_fork.mjs only needs the sample rate from the audio context
    build.onResolve({ filter: /^\.\/audioContext\.mjs$/ }, (args) =>
      args.importer.endsWith('zzfx_fork.mjs') ? { path: path.join(here, 'src/zzfx-context.mjs') } : undefined);
    build.onResolve({ filter: /^@strudel\// }, (args) => {
      const [, name, ...rest] = args.path.split('/');
      const dir = path.join(packages, name);
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
